/* SPDX-License-Identifier: GPL-2.0-only */
/* RXE2 code is immutable after publication. Binding/data pages belong to the
 * process; code ownership and the complete dependency graph survive fork. */
#define RX_MAX 32U
#define RX_DEPS 8U
#define RX_DESCRIPTORS 128U
#define RX_GOT_START 1024U
struct rx_reader { inoptr inode; uint32_t base; unsigned count,failed; uint8_t bytes[BLKSIZE]; };
struct rx_header {
    uint32_t entry,instructions,data,relocations,metadata,initialized;
    unsigned flags,deps,exports,got,imports;
    uint8_t interface[32],identity[32];char name[24];
};
struct rx_export { uint32_t value;unsigned kind;uint8_t type[32]; };
struct rx_module {
    struct rx_header h;
    struct rx_export *exports;
    unsigned dependencies[RX_DEPS],valid,cached;
    unsigned device,inode,uid,gid,mode;
    uint32_t modified,changed;
};
struct rx_plan {
    unsigned count,slots[RX_MAX],banks[RX_MAX],got[RX_MAX];
    uint32_t cells[RX_MAX];inoptr inodes[RX_MAX];
    uint8_t ancestors[RX_MAX][32];
};
static struct rx_module *rx_modules;
static unsigned rx_task_count[PTABSIZE],rx_tasks[PTABSIZE][RX_MAX];
static void rx_reset(struct rx_reader *r,inoptr inode) {r->inode=inode;r->base=0xffffffffUL;r->count=r->failed=0;}
static unsigned rx_byte(struct rx_reader *r,uint32_t pos) {
    uint32_t base=pos&~(uint32_t)BLKMASK;
    if(base!=r->base) {
        udata.u_offset=base;udata.u_count=BLKSIZE;udata.u_base=r->bytes;udata.u_sysio=1;
        readi(r->inode,0);r->base=base;r->count=(unsigned)udata.u_done;
    }
    if((pos&BLKMASK)>=r->count) {r->failed=1;return 0;}
    return r->bytes[(unsigned)pos&BLKMASK]&255;
}
static unsigned rx_word(struct rx_reader *r,uint32_t pos) {return rx_byte(r,pos)|(rx_byte(r,pos+1)<<8);}
static uint32_t rx_long(struct rx_reader *r,uint32_t pos) {uint32_t n=rx_word(r,pos);return n|((uint32_t)rx_word(r,pos+2)<<16);}
static void rx_bytes(struct rx_reader *r,uint32_t pos,void *target,unsigned count) {
    uint8_t *p=target;while(count--)*p++=rx_byte(r,pos++);
}
static int rx_terminated(const char *p,unsigned size) {while(size--)if(!*p++)return 1;return 0;}
static int rx_name_equal(const char *a,const char *b) {while(*a&&*a==*b){++a;++b;}return *a==*b;}
static int rx_header(struct rx_reader *r,struct rx_header *h) {
    uint32_t remaining,minimum;
    memset(h,0,sizeof(*h));
    if(r->inode->c_node.i_size<128 || rx_word(r,0)!=0x5852 || rx_word(r,2)!=0x3245 || rx_word(r,4)!=BANK_CELLS) return -1;
    h->flags=rx_word(r,6);h->entry=rx_long(r,8);h->instructions=rx_long(r,12);
    h->data=rx_long(r,16);h->relocations=rx_long(r,20);h->metadata=rx_long(r,24);
    h->deps=rx_word(r,28);h->exports=rx_word(r,30);h->got=rx_word(r,32);h->imports=rx_word(r,34);
    rx_bytes(r,36,h->interface,32);rx_bytes(r,68,h->identity,32);h->initialized=rx_long(r,100);rx_bytes(r,104,h->name,24);
    minimum=128UL+h->deps*160UL+h->exports*104UL+h->imports*40UL+h->got*12UL;
    if(h->flags>1 || h->deps>RX_DEPS || h->exports>1024 || h->got>16000 || h->imports>8192 ||
       !h->instructions || h->instructions>(uint32_t)(code_limit-RIPPLE_CODE_FIRST)*BANK_CELLS ||
       (h->entry>=h->instructions && !(h->flags && h->entry==0xffffffffUL)) ||
       h->data>=16UL*BANK_CELLS || h->initialized>h->data || !rx_terminated(h->name,24) ||
       h->metadata<minimum || h->metadata%256 || h->metadata>r->inode->c_node.i_size) return -1;
    remaining=r->inode->c_node.i_size-h->metadata;
    if(h->instructions>remaining/8) return -1;
    remaining-=h->instructions*8;
    if(h->initialized>remaining || h->relocations>h->instructions) return -1;
    remaining-=h->initialized;
    return r->failed || remaining%16 || h->relocations!=remaining/16 ? -1:0;
}
static void *rx_allocate(unsigned cells) {
    void *p;irqflags_t flags=di();p=ripple_mmu_kernel_alloc(cells);irqrestore(flags);
    if(!p&&rx_reclaim()){flags=di();p=ripple_mmu_kernel_alloc(cells);irqrestore(flags);}return p;
}
static void rx_drop(unsigned slot) {
    if(rx_modules) {
        struct rx_module *m=&rx_modules[slot];
        unsigned i,count=m->cached?m->h.deps:0,dependencies[RX_DEPS];
        memcpy(dependencies,m->dependencies,count*sizeof(unsigned));
        if(m->exports)ripple_mmu_kernel_free(m->exports,m->h.exports*sizeof(struct rx_export));
        memset(m,0,sizeof(*m));
        for(i=0;i<count;i++)code_drop(dependencies[i]);
    }
}
static unsigned rx_reclaim(void) {
    unsigned slot,changed,total=0;
    if(!rx_modules)return 0;
    do {
        changed=0;
        for(slot=0;slot<CODE_SLOTS;slot++)if(code_refs[slot]==1 && rx_modules[slot].cached) {
            code_drop(slot);++changed;++total;
        }
    }while(changed);
    return total;
}
static unsigned rx_mode(unsigned task) {return rx_task_count[task];}
static void rx_reset_tasks(void) {rx_modules=NULL;memset(rx_task_count,0,sizeof(rx_task_count));}
static void rx_free_task(unsigned task) {
    unsigned i,count=rx_task_count[task];rx_task_count[task]=0;
    for(i=0;i<count;i++)code_drop(rx_tasks[task][i]);
    task_code[task]=-1;
}
static void rx_fork_task(unsigned parent,unsigned child) {
    unsigned i;rx_task_count[child]=rx_task_count[parent];
    for(i=0;i<rx_task_count[parent];i++) {rx_tasks[child][i]=rx_tasks[parent][i];++code_refs[rx_tasks[child][i]];}
    task_code[child]=task_code[parent];
}
static int rx_valid(unsigned task,uint32_t pointer) {
    unsigned i,bank=pointer>>16,off=(unsigned)pointer;
    if(off>=BANK_CELLS)return 0;
    for(i=0;i<rx_task_count[task];i++) {
        unsigned slot=rx_tasks[task][i],base=code_base[slot]-RIPPLE_CODE_FIRST;
        if(bank>=base && (uint32_t)(bank-base)*BANK_CELLS+off<code_size[slot])return 1;
    }
    return 0;
}
static uint32_t rx_code(unsigned slot,uint32_t index) {
    return ((uint32_t)(code_base[slot]-RIPPLE_CODE_FIRST+index/BANK_CELLS)<<16)|(index%BANK_CELLS);
}
static uint32_t rx_imports(const struct rx_header *h) {return 128UL+h->deps*160UL+h->exports*104UL;}
static uint32_t rx_got(const struct rx_header *h) {return rx_imports(h)+h->imports*40UL;}
static struct rx_export *rx_import(struct rx_reader *r,struct rx_module *m,unsigned index,unsigned *owner) {
    uint32_t pos;unsigned dep,symbol,kind;uint8_t type[32];struct rx_module *library;
    if(index>=m->h.imports)return NULL;
    pos=rx_imports(&m->h)+index*40UL;dep=rx_word(r,pos);symbol=rx_word(r,pos+2);kind=rx_word(r,pos+4);rx_bytes(r,pos+8,type,32);
    if(dep>=m->h.deps)return NULL;
    *owner=m->dependencies[dep];library=&rx_modules[*owner];
    if(symbol>=library->h.exports || kind>1 || library->exports[symbol].kind!=kind ||
       memcmp(type,library->exports[symbol].type,32))return NULL;
    return &library->exports[symbol];
}
static int rx_bindings_valid(struct rx_reader *r,struct rx_module *m) {
    unsigned i,owner,kind;uint32_t pos,value,limit,magnitude;long addend;
    struct rx_export *symbol;
    for(i=0;i<m->h.imports;i++)if(!rx_import(r,m,i,&owner))return 0;
    for(i=0;i<m->h.got;i++) {
        pos=rx_got(&m->h)+i*12UL;kind=rx_word(r,pos);value=rx_long(r,pos+4);addend=(long)rx_long(r,pos+8);
        if(kind>1)return 0;
        if(kind) {
            if(value>=m->h.imports || !(symbol=rx_import(r,m,(unsigned)value,&owner)) || symbol->kind)return 0;
            value=symbol->value;limit=rx_modules[owner].h.data;
        } else limit=m->h.data;
        if(addend<0) {magnitude=0UL-(uint32_t)addend;if(magnitude>value)return 0;value-=magnitude;}
        else {if((uint32_t)addend>0xffffffffUL-value)return 0;value+=(uint32_t)addend;}
        if(value>limit)return 0;
    }
    return !r->failed;
}
static inoptr rx_open(const char *path) {
    char saved[96],*scratch=(char *)(PROGBASE+1024);inoptr inode;
    if(ripple_mmu_validate((unsigned long)scratch,96,1)!=96)return NULL;
    memcpy(saved,scratch,96);memcpy(scratch,path,96);inode=n_open(scratch,NULL);memcpy(scratch,saved,96);return inode;
}
static void rx_data(struct rx_reader *reader,unsigned base,uint32_t start,uint32_t size) {
    uint32_t done=0,pointer;
    unsigned fast=RIPPLE_BULK_IO,offset,count,bank,off,run,status;
    while(done<size) {
        (void)rx_byte(reader,start+done);
        if(reader->failed)panic("module data read");
        if(fast) {
            offset=(unsigned)(start+done)&BLKMASK;
            count=reader->count-offset;
            if((uint32_t)count>size-done)count=(unsigned)(size-done);
            pointer=((uint32_t)(base+done/BANK_CELLS)<<16)|(done%BANK_CELLS);
            if(!count||ripple_mmu_span(pointer,1,&bank,&off,&run))panic("module data destination");
            if(count>run)count=run;
            pointer=(uint32_t)(reader->bytes+offset);
            {irqflags_t flags=di();status=vm_bulk_copy(pointer>>16,(unsigned)pointer,bank,off,count,VM_BULK_OCTETS);irqrestore(flags);}
            if(status==VM_DEVICE_OK){done+=count;continue;}
            if(status!=VM_DEVICE_UNKNOWN_COMMAND&&status!=VM_DEVICE_UNSUPPORTED)panic("module data transfer");
            fast=0;
        }
        dmem_store(base+(unsigned)(done/BANK_CELLS),(unsigned)(done%BANK_CELLS),rx_byte(reader,start+done));++done;
    }
}
static int rx_load(struct rx_plan *plan,inoptr inode,unsigned depth,const char *name,const uint8_t *interface) {
    struct rx_reader reader;struct rx_header h;struct rx_module *m;
    unsigned dependencies[RX_DEPS],i,j,slot,kind,owner,cells[4],fast=RIPPLE_BULK_IO;int child,reserved=-1;
    uint32_t pos,index,value,target;char depname[28],path[96];uint8_t expected[32];struct rx_export *symbol;
    rx_reset(&reader,inode);
    if(depth>=RX_MAX || rx_header(&reader,&h))goto invalid;
    if((name && (!h.flags || !rx_name_equal(name,h.name) || memcmp(interface,h.interface,32))) || (!name&&h.flags))goto invalid;
    if(h.flags)for(i=0;i<plan->count;i++) {
        m=&rx_modules[plan->slots[i]];
        if(m->h.flags&&rx_name_equal(m->h.name,h.name)&&
           (memcmp(m->h.identity,h.identity,32)||m->device!=inode->c_dev||m->inode!=inode->c_num))goto invalid;
    }
    for(i=0;i<depth;i++)if(!memcmp(plan->ancestors[i],h.identity,32))goto invalid;
    memcpy(plan->ancestors[depth],h.identity,32);
    for(i=0;i<h.deps;i++) {
        inoptr dependency;
        pos=128UL+i*160UL;rx_bytes(&reader,pos,depname,28);rx_bytes(&reader,pos+28,path,96);rx_bytes(&reader,pos+124,expected,32);
        if(!rx_terminated(depname,28)||!rx_terminated(path,96)||path[0]!='/'||!depname[0])goto invalid;
        dependency=rx_open(path);if(!dependency)goto failed;
        if(getmode(dependency)!=MODE_R(F_REG) || !(getperm(dependency)&OTH_RD) ||
           (fs_tab[dependency->c_super].m_flags&MS_NOEXEC)) {i_deref(dependency);udata.u_error=EACCES;goto failed;}
        child=rx_load(plan,dependency,depth+1,depname,expected);if(child<0)goto failed;
        dependencies[i]=plan->slots[child];
    }
    if(!rx_modules) {
        rx_modules=rx_allocate(sizeof(struct rx_module)*CODE_SLOTS);
        if(!rx_modules){udata.u_error=ENOMEM;goto failed;}
    }
    for(slot=0;slot<CODE_SLOTS;slot++) {
        m=&rx_modules[slot];
        /* A digest supplied by an executable is not an authentication token.
         * Never reuse text from an unrelated file claiming the same identity. */
        if(!code_refs[slot]||!m->valid||m->device!=inode->c_dev||m->inode!=inode->c_num||
           m->uid!=inode->c_node.i_uid||m->gid!=inode->c_node.i_gid||m->mode!=inode->c_node.i_mode||
           m->modified!=inode->c_node.i_mtime||m->changed!=inode->c_node.i_ctime||memcmp(m->h.identity,h.identity,32)||
           memcmp(&rx_modules[slot].h,&h,sizeof(h)))continue;
        for(i=0;i<h.deps && rx_modules[slot].dependencies[i]==dependencies[i];i++);
        if(i==h.deps)break;
    }
    if(slot<CODE_SLOTS) {
        if(!rx_bindings_valid(&reader,&rx_modules[slot]))goto invalid;
        for(i=0;i<plan->count;i++)if(plan->slots[i]==slot){i_deref(inode);return i;}
        if(plan->count==RX_MAX)goto invalid;
        ++code_refs[slot];goto complete;
    }
    if(plan->count==RX_MAX)goto invalid;
    reserved=code_reserve(h.instructions);if(reserved<0){udata.u_error=ENOMEM;goto failed;}
    slot=reserved;m=&rx_modules[slot];memset(m,0,sizeof(*m));m->h=h;
    m->device=inode->c_dev;m->inode=inode->c_num;m->uid=inode->c_node.i_uid;m->gid=inode->c_node.i_gid;
    m->mode=inode->c_node.i_mode;m->modified=inode->c_node.i_mtime;m->changed=inode->c_node.i_ctime;
    memcpy(m->dependencies,dependencies,h.deps*sizeof(unsigned));
    if(h.exports) {
        m->exports=rx_allocate(h.exports*sizeof(struct rx_export));
        if(!m->exports){udata.u_error=ENOMEM;goto failed;}
    }
    for(i=0;i<h.exports;i++) {
        pos=128UL+h.deps*160UL+i*104UL;value=rx_long(&reader,pos+64);kind=rx_word(&reader,pos+68);
        if(kind>1 || (kind ? value>=h.instructions : value>=h.data))goto invalid;
        m->exports[i].value=kind?rx_code(slot,value):value;m->exports[i].kind=kind;
        rx_bytes(&reader,pos+72,m->exports[i].type,32);
    }
    if(!rx_bindings_valid(&reader,m))goto invalid;
    /* Preflight all patches before making code visible or changing user RAM. */
    for(index=0;index<h.relocations;index++) {
        pos=h.metadata+h.instructions*8+h.initialized+index*16;
        kind=rx_word(&reader,pos);value=rx_long(&reader,pos+4);target=rx_long(&reader,pos+8);j=(unsigned)target;
        if(kind>6 || value>=h.instructions || (kind>=4 && (target>=h.imports)) || (kind>=4 &&
           (!(symbol=rx_import(&reader,m,j,&owner))||!symbol->kind)))goto invalid;
        i=rx_byte(&reader,h.metadata+value*8);
        if(i!=((kind==0||kind==4)?19:14))goto invalid;
    }
    if(reader.failed)goto invalid;
    /* Upload each code block once. Cache hits above never execute STORC. */
    index=0;
    while(index<h.instructions) {
        unsigned count,status,bank,off;uint32_t pointer;
        pos=h.metadata+index*8;(void)rx_byte(&reader,pos);
        off=(unsigned)pos&BLKMASK;count=(reader.count-off)/8;
        if((uint32_t)count>h.instructions-index)count=(unsigned)(h.instructions-index);
        if(count>VM_BULK_MAX_INSTRUCTIONS)count=VM_BULK_MAX_INSTRUCTIONS;
        if(!count)goto invalid;
        bank=code_base[slot]+(unsigned)(index/BANK_CELLS);i=(unsigned)(index%BANK_CELLS);
        pointer=(unsigned long)(reader.bytes+off);
        status=VM_DEVICE_UNSUPPORTED;
        if(fast){irqflags_t flags=di();status=vm_bulk_copy(pointer>>16,(unsigned)pointer,bank,i,count,VM_BULK_CODE_OCTETS);irqrestore(flags);}
        if(status==VM_DEVICE_OK){index+=count;continue;}
        if(status!=VM_DEVICE_UNKNOWN_COMMAND&&status!=VM_DEVICE_UNSUPPORTED)goto invalid;
        fast=0;
        cells[0]=rx_byte(&reader,pos);cells[1]=rx_word(&reader,pos+2);cells[2]=rx_word(&reader,pos+4);cells[3]=rx_word(&reader,pos+6);
        imem_store(bank,i,cells);++index;
    }
    for(index=0;index<h.relocations;index++) {
        unsigned bank,off;
        pos=h.metadata+h.instructions*8+h.initialized+index*16;
        kind=rx_word(&reader,pos);value=rx_long(&reader,pos+4);j=rx_long(&reader,pos+8);
        pos=h.metadata+value*8;
        for(i=0;i<4;i++)cells[i]=rx_word(&reader,pos+i*2UL);
        if(kind<2) {
            if((kind==0 && ((uint32_t)cells[2]*BANK_CELLS+cells[3]>=h.instructions || cells[3]>=BANK_CELLS)) ||
               (kind==1 && cells[2]>=(h.instructions+BANK_CELLS-1)/BANK_CELLS))goto invalid;
            cells[2]+=code_base[slot]-RIPPLE_CODE_FIRST;
        } else if(kind<4)cells[2]=RX_DESCRIPTORS+slot*2+(kind==3);
        else {
            symbol=rx_import(&reader,m,j,&owner);bank=symbol->value>>16;off=(unsigned)symbol->value;
            if(kind==4){cells[2]=bank;cells[3]=off;}
            else cells[2]=kind==5?bank:off;
        }
        imem_store(code_base[slot]+(unsigned)(value/BANK_CELLS),(unsigned)(value%BANK_CELLS),cells);
    }
    if(reader.failed)goto invalid;
    m->valid=1;m->cached=1;++code_refs[slot];
    for(i=0;i<h.deps;i++)++code_refs[m->dependencies[i]];
complete:
    i=plan->count++;plan->slots[i]=slot;plan->inodes[i]=inode;return i;
invalid:
    udata.u_error=ENOEXEC;
failed:
    if(reserved>=0)code_drop(reserved);
    i_deref(inode);return -1;
}

static arg_t rx_exec(inoptr inode) {
    struct rx_plan plan;struct s_argblk args,env;struct rx_reader reader;
    unsigned i,j,task=(unsigned)(udata.u_ptab-ptab),bank=USER_GP+1,got=RX_GOT_START,owner,argc;
    int root;uint32_t pos,n,value,entry,end;uint8_t **av,**ev;unsigned *bindings=(unsigned *)((uint32_t)USER_GP<<16);
    struct rx_module *m;struct rx_export *symbol;
    memset(&plan,0,sizeof(plan));
    if(rargs((uint8_t **)udata.u_argn1,&args)||rargs((uint8_t **)udata.u_argn2,&env)) {i_deref(inode);return -1;}
    if((uint32_t)args.a_arglen+env.a_arglen+(args.a_argc+env.a_argc+2UL)*sizeof(uptr_t)>RIPPLE_ARG_MAX) {
        i_deref(inode);udata.u_error=E2BIG;return -1;
    }
    root=rx_load(&plan,inode,0,NULL,NULL);if(root<0)goto failed;
    for(i=0;i<plan.count;i++) {
        m=&rx_modules[plan.slots[i]];plan.banks[i]=bank;plan.cells[i]=m->h.data;plan.got[i]=got;
        if(m->h.got>(BANK_CELLS-got)/2 || bank>USER_HEAP_LAST)goto invalid;
        got+=m->h.got*2;bank+=(unsigned)((m->h.data+BANK_CELLS-1)/BANK_CELLS);
    }
    if(ripple_mmu_exec_layout(task,plan.banks,plan.cells,plan.count,got<1152?1152:got)) {
        rx_reclaim();
        if(ripple_mmu_exec_layout(task,plan.banks,plan.cells,plan.count,got<1152?1152:got)){udata.u_error=ENOMEM;goto failed;}
    }
    /* After committing the new page map, file I/O failure is unrecoverable,
     * as in the legacy RXE1 path. Every dependency/relocation was checked above. */
    for(i=0;i<plan.count;i++) {
        unsigned slot=plan.slots[i];m=&rx_modules[slot];rx_reset(&reader,plan.inodes[i]);
        bindings[RX_DESCRIPTORS+slot*2]=plan.banks[i];bindings[RX_DESCRIPTORS+slot*2+1]=plan.got[i];
        if(i!=(unsigned)root && m->h.entry!=0xffffffffUL) {
            value=rx_code(slot,m->h.entry);j=bindings[0]++;bindings[1+j*2]=(unsigned)value;bindings[2+j*2]=value>>16;
        }
        rx_data(&reader,plan.banks[i],m->h.metadata+m->h.instructions*8,m->h.initialized);
        for(j=0;j<m->h.got;j++) {
            long addend;unsigned kind,base;
            pos=rx_got(&m->h)+j*12UL;kind=rx_word(&reader,pos);value=rx_long(&reader,pos+4);addend=(long)rx_long(&reader,pos+8);
            if(kind) {
                symbol=rx_import(&reader,m,(unsigned)value,&owner);value=symbol->value;
                for(base=0;base<plan.count&&plan.slots[base]!=owner;base++);
                if(base==plan.count)panic("module data owner");
            } else base=i;
            if((addend<0 && (uint32_t)(-addend)>value) || (addend>0 && value+(uint32_t)addend<value))panic("module pointer overflow");
            value+=addend;
            if(value>plan.cells[base])panic("module data relocation");
            bindings[plan.got[i]+j*2]=(unsigned)(value%BANK_CELLS);
            bindings[plan.got[i]+j*2+1]=plan.banks[base]+(unsigned)(value/BANK_CELLS);
        }
        if(reader.failed)panic("module data read");
    }
    av=wargs((uint8_t *)PROGTOP,&args,(int *)&argc);ev=wargs((uint8_t *)av,&env,NULL);
    m=&rx_modules[plan.slots[plan.count-1]];end=((plan.banks[plan.count-1]+m->h.data/BANK_CELLS)<<16)|(m->h.data%BANK_CELLS);
    udata.u_top=PROGTOP;udata.u_ptab->p_top=PROGTOP;udata.u_break=end;udata.u_codebase=PROGBASE;
    ripple_argc=argc;ripple_argv=(uint32_t)av;ripple_envp=(uint32_t)ev;
    ripple_gfx_exit(udata.u_ptab);ripple_pcm_exit(udata.u_ptab);
    for(i=0;i<UFTSIZE;i++)if(udata.u_cloexec&(1UL<<i))doclose(i);
    udata.u_cloexec=0;memset(udata.u_sigvec,0,sizeof(udata.u_sigvec));udata.u_sigtramp=0;udata.u_cursig=0;udata.u_error=0;
    memset(udata.u_name,0,sizeof(udata.u_name));
    if(argc){uint8_t *name=(uint8_t *)ugetp(av);for(i=0;i<sizeof(udata.u_name);i++){int c=ugetc(name+i);if(c<0)break;udata.u_name[i]=c;if(!c)break;}}
    udata.u_ptab->p_flags|=PFL_EXECED;memcpy(udata.u_ptab->p_name,udata.u_name,sizeof(udata.u_name));
    entry=rx_code(plan.slots[root],rx_modules[plan.slots[root]].h.entry);
    ripple_code_free(udata.u_ptab);rx_task_count[task]=plan.count;task_code[task]=plan.slots[root];
    ripple_mmu_code_clear(task);
    for(i=0;i<plan.count;i++) {
        unsigned slot=plan.slots[i];rx_tasks[task][i]=slot;
        ripple_mmu_code_add(task,code_base[slot]-RIPPLE_CODE_FIRST,code_base[slot],code_size[slot]);i_deref(plan.inodes[i]);
    }
    ripple_mmu_flush_code();ripple_exec_bank=entry>>16;ripple_exec_pc=(unsigned)entry;
    __hard_di();udata.u_insys=0;proc_setstate(udata.u_ptab,P_RUNNING);proc_sched_switch();ripple_enter_user();panic("module exec returned");
invalid:
    udata.u_error=ENOEXEC;
failed:
    for(i=0;i<plan.count;i++){code_drop(plan.slots[i]);i_deref(plan.inodes[i]);}
    return -1;
}
