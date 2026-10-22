/* UEFI PCI resource metadata only. No controller connection, BAR sizing,
 * attribute changes, AML, hardware programming or post-EBS protocol calls.
 * Interface/layout: UEFI 2.10 14.2.18 / 14.4.18 and EDK2 PciRootBridgeIo.h,
 * PciIo.h. Configuration buffers are borrowed; GetBarAttributes buffers and
 * LocateHandleBuffer arrays are caller-owned only on EFI_SUCCESS and freed
 * before handoff. Optional BAR metadata is not evidence of live BAR absence. */
#include "pci-uefi.h"

#define HANDLES 256U
#define ROOTS 16U
#define DESCRIPTORS 64U
#define EFI_NOT_FOUND EFI_ERROR(14)
#define EFI_UNSUPPORTED EFI_ERROR(3)
#define BY_PROTOCOL 2U
#define DWORD_WIDTH 2U
#define TARGET_BDF 0x00aaU

typedef struct { void *read, *write; } Access;
typedef struct RootIo RootIo;
struct RootIo {
    Handle parent;
    void *poll_mem, *poll_io;
    Access mem, io;
    struct { Status (*read)(RootIo *, uint32_t, uint64_t, size_t, void *); void *write; } pci;
    void *copy_mem, *map, *unmap, *allocate_buffer, *free_buffer, *flush;
    void *get_attributes, *set_attributes;
    Status (*configuration)(RootIo *, void **);
    uint32_t segment;
};
typedef struct PciIo PciIo;
struct PciIo {
    void *poll_mem, *poll_io;
    Access mem, io;
    struct { Status (*read)(PciIo *, uint32_t, uint32_t, size_t, void *); void *write; } pci;
    void *copy_mem, *map, *unmap, *allocate_buffer, *free_buffer, *flush;
    Status (*get_location)(PciIo *, size_t *, size_t *, size_t *, size_t *);
    void *attributes;
    Status (*get_bar_attributes)(PciIo *, uint8_t, uint64_t *, void **);
    void *set_bar_attributes;
    uint64_t rom_size;
    void *rom_image;
};
_Static_assert(offsetof(RootIo, configuration) == 136, "root Configuration slot");
_Static_assert(offsetof(RootIo, segment) == 144, "root segment slot");
_Static_assert(offsetof(PciIo, get_location) == 112, "PCI GetLocation slot");
_Static_assert(offsetof(PciIo, get_bar_attributes) == 128, "PCI GetBarAttributes slot");
_Static_assert(sizeof(PciBootWindow) == 24 && sizeof(PciBootSpan) == 24, "snapshot records");
static const Guid root_guid = {0x2f707ebb,0x4a1a,0x11d4,{0x9a,0x38,0,0x90,0x27,0x3f,0xc1,0x4d}};
static const Guid pci_guid = {0x4cf5b200,0x68b8,0x4ca5,{0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
typedef struct { RootIo *io; uint8_t first, last; } Root;
typedef struct {
    BootServices *services;
    PciBootResources *out;
    Root roots[ROOTS];
    unsigned root_count;
} Capture;
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2)<<16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p+4)<<32; }
static void zero(void *p, size_t bytes) { volatile uint8_t *b=p;while(bytes--)*b++=0; }
static int problem(Capture *c, const char *reason) {
    c->out->status=PCI_BOOT_INCOMPLETE;
    if (!c->out->reason[0]) {
        unsigned i=0;
        while(reason[i] && i+1<sizeof(c->out->reason)) { c->out->reason[i]=reason[i];++i; }
        c->out->reason[i]=0;
    }
    return 0;
}
/* Copy raw metadata into the existing persistent reason field. No firmware
 * pointer or printf dependency survives handoff, and no BootInfo ABI changes. */
static int descriptor_problem(Capture *c,const char *why,int root,const uint8_t *p) {
    char reason[96];unsigned at=0;
    while(*why && at<16)reason[at++]=*why++;
    const char *source=root?" ROOT t=":" BAR t=";
    while(*source)reason[at++]=*source++;
    static const char digits[]="0123456789abcdef";
    reason[at++]=digits[p[3]>>4];reason[at++]=digits[p[3]&15];
    const unsigned offsets[]={14,22,38};
    const char *labels[]={" min="," max="," len="};
    for(unsigned n=0;n<3;n++) {
        const char *label=labels[n];while(*label)reason[at++]=*label++;
        uint64_t value=u64(p+offsets[n]);
        for(int shift=60;shift>=0;shift-=4)reason[at++]=digits[value>>shift&15];
    }
    reason[at]=0;
    return problem(c,reason);
}
static int device_problem(Capture *c,const char *message,uint32_t segment,uint16_t bdf) {
    char reason[96];unsigned at=0;
    while(*message && at<66)reason[at++]=*message++;
    static const char digits[]="0123456789abcdef";
    const char *label=" seg=";while(*label)reason[at++]=*label++;
    for(int shift=28;shift>=0;shift-=4)reason[at++]=digits[segment>>shift&15];
    label=" bdf=";while(*label)reason[at++]=*label++;
    for(int shift=12;shift>=0;shift-=4)reason[at++]=digits[bdf>>shift&15];
    reason[at]=0;return problem(c,reason);
}
static int interval(uint64_t base,uint64_t length,uint64_t max) {
    return length && length-1<=UINT64_MAX-base && base+length-1==max;
}
static int add_span(Capture *c,uint32_t segment,uint16_t bdf,uint8_t bar,
                    uint8_t kind,uint64_t base,uint64_t length) {
    if (!length || length>UINT64_MAX-base)
        return problem(c,"PCI occupied interval overflow/zero");
    for(unsigned i=0;i<c->out->span_count;i++) {
        PciBootSpan *s=&c->out->occupied[i];
        if(s->segment==segment && s->bdf==bdf && s->bar==bar && s->kind==kind &&
                s->base==base && s->length==length)return 1;
    }
    if(c->out->span_count==PCI_BOOT_SPANS)return problem(c,"PCI occupied snapshot capacity");
    c->out->occupied[c->out->span_count++]=(PciBootSpan){base,length,segment,bdf,bar,kind};
    if(segment==0 && bdf==TARGET_BDF && bar==0 && kind==PCI_BOOT_BAR) {
        if(c->out->target_bar0_base && (c->out->target_bar0_base!=base ||
                c->out->target_bar0_length!=length))return problem(c,"target BAR0 resource ambiguous");
        c->out->target_bar0_base=base;c->out->target_bar0_length=length;
    }
    return 1;
}
static int reserve_unknown(Capture *c,uint32_t segment,uint16_t bdf,unsigned bar,
                           unsigned type,uint64_t base) {
    if(!base)return 1;
    /* PCI BAR sizes are powers of two and bases are size-aligned. The lowest
     * set bit therefore bounds the largest decoder at this nonzero base.
     * This inventories an assigned BAR without sizing an active device.
     * UINT64_MAX excludes the unrepresentable last byte, above our <2^47
     * allocator limit. Legacy 1MiB BARs, like Linux decode_bar(), are treated
     * conservatively as 32-bit. Never cache this as an exact target resource. */
    uint64_t size=base&(~base+1);
    uint64_t end=size>UINT64_MAX-base?UINT64_MAX:base+size;
    if(type!=4 && end>(1ULL<<32))end=1ULL<<32;
    if(base>=end)return device_problem(c,"unknown BAR reservation bounds",segment,bdf);
    return add_span(c,segment,bdf,(uint8_t)bar,PCI_BOOT_UNKNOWN,base,end-base);
}
/* Firmware supplies the pointer, but no length. Bound descriptor count and
 * accept only the two UEFI-defined record types, never a generic AML stream. */
static int resources(Capture *c,const void *buffer,RootIo *root,
                     uint32_t segment,uint16_t bdf,uint8_t bar) {
    if(!buffer)return problem(c,"PCI resource descriptor missing");
    const uint8_t *p=buffer;
    unsigned initial=c->out->window_count;
    uint8_t first[ROOTS],last[ROOTS];
    unsigned buses=0;
    for(unsigned n=0;n<DESCRIPTORS;n++) {
        if(p[0]==0x79) {
            if(root) {
                if(!buses)return problem(c,"root PCI bus range missing");
                unsigned end=c->out->window_count;
                for(unsigned b=0;b<buses;b++) {
                    if(c->root_count==ROOTS)return problem(c,"PCI root snapshot capacity");
                    for(unsigned i=0;i<c->root_count;i++)
                        if(c->roots[i].io->segment==segment && first[b]<=c->roots[i].last &&
                                last[b]>=c->roots[i].first)return problem(c,"overlapping PCI root bus ranges");
                    c->roots[c->root_count++]=(Root){root,first[b],last[b]};
                    for(unsigned i=initial;i<end;i++) {
                        unsigned at=i;
                        if(b) {
                            if(c->out->window_count==PCI_BOOT_WINDOWS)return problem(c,"PCI aperture snapshot capacity");
                            at=c->out->window_count++;c->out->windows[at]=c->out->windows[i];
                        }
                        c->out->windows[at].bus_first=first[b];c->out->windows[at].bus_last=last[b];
                    }
                }
            }
            return 1;
        }
        if(p[0]!=0x8a || u16(p+1)!=0x2b)return problem(c,"unsupported PCI resource descriptor");
        unsigned type=p[3];uint64_t min=u64(p+14),max=u64(p+22),length=u64(p+38);
        if(!length){p+=46;continue;}
        /* Root Configuration gives an actual inclusive end. PCI_IO BAR
         * descriptors differ in real EDK2: PciIoGetBarAttributes writes the
         * BAR Alignment mask to AddrRangeMax (and other firmware leaves it
         * zero). Use the authoritative Min+Len there, never interpret that
         * auxiliary field as a mapped extent or allocation constraint.
         * Upstream: MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c. */
        if(length>UINT64_MAX-min || (root && !interval(min,length,max)))
            return descriptor_problem(c,"bounds",root!=0,p);
        uint64_t end=min+length-1;
        if(type==2 && root) {
            if(min>max || max>255)return descriptor_problem(c,"bus-bounds",1,p);
            if(buses==ROOTS)return problem(c,"PCI root bus range capacity");
            first[buses]=(uint8_t)min;last[buses++]=(uint8_t)max;
        }
        else if(type==0) {
            if(u64(p+30))return descriptor_problem(c,"translation",root!=0,p);
            uint64_t width=u64(p+6);
            if(width!=32 && width!=64)return descriptor_problem(c,"width",root!=0,p);
            if(width==32 && end>UINT32_MAX)return descriptor_problem(c,"32bit-bounds",root!=0,p);
            if(root) {
                if(c->out->window_count==PCI_BOOT_WINDOWS)return problem(c,"PCI aperture snapshot capacity");
                c->out->windows[c->out->window_count++]=(PciBootWindow){
                    min,length,segment,0,0,(p[5]&6)==6,0};
            } else if(min && !(length&(length-1)) && !(min&(length-1))) {
                /* A successful optional firmware query can still describe a
                 * stale/malformed BAR extent (observed: non-power-of-two
                 * SMBus length). PCI decoders are size-aligned powers of two.
                 * Such metadata grants neither exactness nor target-cache
                 * ownership; independent live coverage reserves UNKNOWN and
                 * native sizing can refine it after ExitBootServices. Keep
                 * structural/translation/overflow errors above explicit. */
                if(!add_span(c,segment,bdf,bar,PCI_BOOT_BAR,min,length))return 0;
            }
        } else if(type!=1 && type!=2)return problem(c,"unknown PCI resource type");
        p+=46;
    }
    return problem(c,"unterminated PCI resource descriptors");
}
static int root_contains(Capture *c,uint32_t segment,unsigned bus) {
    unsigned found=0;
    for(unsigned i=0;i<c->root_count;i++)
        if(c->roots[i].io->segment==segment && bus>=c->roots[i].first && bus<=c->roots[i].last)++found;
    return found==1;
}
static int read_device(PciIo *io,unsigned offset,uint32_t *out) {
    return !io->pci.read(io,DWORD_WIDTH,offset,1,out);
}
static int bridge_span(Capture *c,uint32_t segment,uint16_t bdf,uint64_t base,uint64_t max,uint8_t bar) {
    if(base>max)return 1; /* conventional disabled bridge window encoding */
    if(max==UINT64_MAX)return problem(c,"bridge aperture end overflow");
    return add_span(c,segment,bdf,bar,PCI_BOOT_BRIDGE,base,max-base+1);
}
static int bridge_windows(Capture *c,uint32_t segment,uint16_t bdf,const uint32_t config[16]) {
    unsigned header=(config[3]>>16)&127;
    if(header==0) {
        if(config[12]&~0x7ffU)
            /* ROM BARs encode only 32-bit addresses. Without probing an active
             * device, conservatively exclude their entire address domain. */
            return add_span(c,segment,bdf,0xfc,PCI_BOOT_ROM32,1,UINT32_MAX);
        return 1;
    }
    if(header==2)
        /* CardBus memory windows and its single socket BAR are all 32-bit.
         * No active CardBus bridge needs probing for our >4GiB allocator. */
        return add_span(c,segment,bdf,0xfb,PCI_BOOT_UNKNOWN,1,UINT32_MAX);
    if(header!=1)return problem(c,"unsupported PCI header resource inventory");
    /* Linux pci_setup_device identifies programming-interface 1 as a
     * transparent/subtractive bridge. Its downstream decoders can receive
     * traffic outside positive windows, so root-only coverage is insufficient
     * for that topology; do not claim an aperture is collision-free there. */
    if(((config[2]>>8)&255)==1)
        return device_problem(c,"subtractive bridge needs downstream inventory",segment,bdf);
    if((config[14]&~0x7ffU) && !add_span(c,segment,bdf,0xfc,PCI_BOOT_ROM32,1,UINT32_MAX))return 0;
    uint32_t m=config[8],p=config[9];
    /* Nonprefetchable bridge windows are 32-bit; reserved low bits are not
     * part of their extent. Linux pci_read_bridge_mmio masks them too. */
    if(!bridge_span(c,segment,bdf,(uint64_t)(m&0xfff0U)<<16,(m&0xfff00000U)|0xfffffULL,0xfe))return 0;
    unsigned low=p&15,high=(p>>16)&15;
    if(low!=high || low>1)return problem(c,"bridge prefetch window flags");
    uint64_t base=(uint64_t)(p&0xfff0U)<<16,max=(p&0xfff00000U)|0xfffffULL;
    if(low==1){base|=(uint64_t)config[10]<<32;max|=(uint64_t)config[11]<<32;}
    return bridge_span(c,segment,bdf,base,max,0xfd);
}
static int covered(Capture *,uint32_t,uint16_t,unsigned,uint64_t);
static int capture_device(Capture *c,PciIo *io) {
    if(!io || !io->get_location || !io->get_bar_attributes || !io->pci.read)
        return problem(c,"PCI IO protocol methods missing");
    size_t segment,bus,device,function;
    if(io->get_location(io,&segment,&bus,&device,&function) || segment>UINT32_MAX ||
            bus>255 || device>31 || function>7 || !root_contains(c,(uint32_t)segment,(unsigned)bus))
        return problem(c,"PCI device root/location not inventoried");
    uint16_t bdf=(uint16_t)(bus<<8|device<<3|function);
    uint32_t config[16];
    if(!read_device(io,0,&config[0]))return device_problem(c,"PCI vendor read failed",(uint32_t)segment,bdf);
    unsigned vendor=config[0]&65535;
    unsigned present=vendor!=65535 && vendor!=0;
    unsigned bars=6;
    if(present) {
        for(unsigned i=1;i<16;i++)
            if(!read_device(io,i*4,&config[i]))return device_problem(c,"PCI config snapshot failed",(uint32_t)segment,bdf);
        unsigned header=(config[3]>>16)&127;
        bars=header==0?6:header==1?2:header==2?1:0;
        if(!bars)return device_problem(c,"unsupported PCI header resource inventory",(uint32_t)segment,bdf);
        if(!bridge_windows(c,(uint32_t)segment,bdf,config))return 0;
    }
    /* Firmware handles may survive a device becoming absent/inaccessible.
     * Their cached BAR resources still reserve addresses, but absent config
     * registers are not headers, BAR types or ROM-window evidence. */
    for(unsigned bar=0;bar<bars;bar++) {
        unsigned type=0;uint64_t live=0;
        if(present) {
            uint32_t value=config[4+bar];
            if(value&1)continue;
            type=value&6;
            if(type!=0 && type!=2 && type!=4)return device_problem(c,"unsupported endpoint BAR type",(uint32_t)segment,bdf);
            if(type==4 && bar+1>=bars)return device_problem(c,"truncated 64-bit endpoint BAR",(uint32_t)segment,bdf);
            live=value&~15U;
            if(type==4)live|=(uint64_t)config[5+bar]<<32;
        }
        void *descriptor=0;
        Status status=io->get_bar_attributes(io,(uint8_t)bar,0,&descriptor);
        if(!status) {
            int okay=resources(c,descriptor,0,(uint32_t)segment,bdf,(uint8_t)bar);
            Status free_status=descriptor?c->services->free_pool(descriptor):0;
            if(!okay)return 0;
            if(free_status)return problem(c,"PCI BAR descriptor buffer free failed");
        } else {
            /* Outputs on failure transfer no ownership. In EDK2's translation
             * failure path *Resources can retain an already-freed allocation;
             * never free/dereference it unless the call returned success. */
            /* Every query failure is optional metadata failure, including on
             * a stale handle. Mandatory root coverage independently reserves
             * present direct BARs and whole forwarding bridge MMIO windows;
             * downstream traffic cannot escape those windows. Successful
             * stale cached descriptors add reservations, never free space.
             * Do not turn optional failure statuses into a global veto. */
        }
        /* GetBarAttributes can be unsupported even for a present assigned BAR
         * (EDK2's translation lookup is one example). Root coverage must not
         * be preempted by optional metadata failure; reserve the decoder here
         * as well, including assigned devices on buses behind a bridge. */
        if(live && !covered(c,(uint32_t)segment,bdf,bar,live) &&
                !reserve_unknown(c,(uint32_t)segment,bdf,bar,type,live))return 0;
        if(type==4)++bar;
    }
    return 1;
}
static int covered(Capture *c,uint32_t segment,uint16_t bdf,unsigned bar,uint64_t base) {
    for(unsigned i=0;i<c->out->span_count;i++) {
        const PciBootSpan *s=&c->out->occupied[i];
        if(s->segment==segment && s->bdf==bdf && s->bar==bar && s->kind==PCI_BOOT_BAR &&
                base>=s->base && base-s->base<s->length)return 1;
    }
    return 0;
}
/* Check the directly attached bus even for devices firmware did not expose as
 * PCI_IO handles. An unassigned target may have no handle; an assigned hidden
 * endpoint cannot be silently omitted from the collision inventory. */
static int root_coverage(Capture *c,const Root *root) {
    if(!root->io->pci.read)return problem(c,"root config read unavailable");
    for(unsigned slot=0;slot<256;slot++) {
        uint16_t bdf=(uint16_t)((unsigned)root->first<<8|slot);
        uint64_t address=(uint64_t)root->first<<24|(uint64_t)(slot>>3)<<16|(uint64_t)(slot&7)<<8;
        uint32_t config[16];
        Status status=root->io->pci.read(root->io,DWORD_WIDTH,address,1,&config[0]);
        if(status)return device_problem(c,"direct root config read failed",root->io->segment,bdf);
        if((config[0]&65535)==65535 || !(config[0]&65535))continue;
        for(unsigned i=1;i<16;i++)
            if(root->io->pci.read(root->io,DWORD_WIDTH,address+i*4,1,&config[i]))
                return device_problem(c,"direct root config bounds/read failed",root->io->segment,bdf);
        unsigned header=(config[3]>>16)&127;
        unsigned bars=header==0?6:header==1?2:header==2?1:0;
        if(!bars)return problem(c,"unsupported direct root PCI header");
        if(!bridge_windows(c,root->io->segment,bdf,config))return 0;
        for(unsigned bar=0;bar<bars;bar++) {
            uint32_t low=config[4+bar];
            if(low&1)continue;
            unsigned type=low&6;
            if(type!=0 && type!=2 && type!=4)return device_problem(c,"direct root BAR type unsupported",root->io->segment,bdf);
            if(type==4 && bar+1>=bars)return problem(c,"direct root 64-bit BAR truncated");
            uint64_t base=low&~15U;
            if(type==4)base|=(uint64_t)config[5+bar]<<32;
            if(base && !covered(c,root->io->segment,bdf,bar,base)) {
                if(!reserve_unknown(c,root->io->segment,bdf,bar,type,base))return 0;
            }
            if(type==4)++bar;
        }
    }
    return 1;
}
void pci_uefi_capture(BootServices *services,BootInfo *boot) {
    Capture c;zero(&c,sizeof(c));c.services=services;c.out=&boot->pci;
    zero(c.out,sizeof(*c.out));c.out->version=1;
    if(!services || !services->locate_handle_buffer || !services->handle_protocol || !services->free_pool) {
        (void)problem(&c,"UEFI PCI snapshot services missing");return;
    }
    Handle *handles=0;size_t count=0;
    Status status=services->locate_handle_buffer(BY_PROTOCOL,&root_guid,0,&count,&handles);
    if(status || !handles || !count || count>HANDLES) {
        if(!status && handles)(void)services->free_pool(handles);
        (void)problem(&c,"PCI root handles missing/over capacity");return;
    }
    int okay=1;
    for(size_t i=0;i<count && okay;i++) {
        RootIo *root=0;void *descriptors=0;
        if(services->handle_protocol(handles[i],&root_guid,(void **)&root) || !root || !root->configuration)
            okay=problem(&c,"root PCI protocol unavailable");
        else if(root->configuration(root,&descriptors))okay=problem(&c,"root PCI configuration unavailable");
        else okay=resources(&c,descriptors,root,root->segment,0,0);
        /* Root Configuration's buffer is read-only borrowed firmware state. */
    }
    if(services->free_pool(handles))okay=problem(&c,"PCI root handle buffer free failed");
    if(!okay || !c.root_count)return;
    handles=0;count=0;
    status=services->locate_handle_buffer(BY_PROTOCOL,&pci_guid,0,&count,&handles);
    if(status==EFI_NOT_FOUND) {handles=0;count=0;}
    else if(status || !handles || count>HANDLES) {
        if(!status && handles)(void)services->free_pool(handles);
        (void)problem(&c,"PCI IO handles missing/over capacity");return;
    }
    for(size_t i=0;i<count && okay;i++) {
        PciIo *pci=0;
        if(services->handle_protocol(handles[i],&pci_guid,(void **)&pci))okay=problem(&c,"PCI IO handle query failed");
        else okay=capture_device(&c,pci);
    }
    if(handles && services->free_pool(handles))okay=problem(&c,"PCI IO handle buffer free failed");
    if(!okay)return;
    for(unsigned i=0;i<c.root_count;i++)if(!root_coverage(&c,&c.roots[i]))return;
    if(!c.out->window_count){(void)problem(&c,"PCI root MMIO apertures absent");return;}
    c.out->status=PCI_BOOT_COMPLETE;
}
