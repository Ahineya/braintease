/* Floating tabs compose retained terminal surfaces back to front. PTYs remain
 * independent; obscured output can never paint over a higher window. */
static void commit_pane(struct pane *p){
    unsigned x,y;int fresh=0;
    if(p->committed_w!=p->w||p->committed_h!=p->h){
        struct vt_cell *next=malloc((unsigned long)p->w*p->h*sizeof(*next));
        if(!next){notice("Not enough memory for a floating window frame");return;}
        for(y=0;y<p->h;y++)for(x=0;x<p->w;x++)
            next[y*p->w+x]=p->committed&&y<p->committed_h&&x<p->committed_w?p->committed[y*p->committed_w+x]:blank_cell;
        free(p->committed);p->committed=next;p->committed_w=p->w;p->committed_h=p->h;fresh=1;
    }
    /* A partially received synchronized update must remain invisible even if
     * another window moves or changes stacking order during that update. */
    if(p->vt.synchronized)return;
    for(y=0;y<p->h;y++)if(fresh||full||p->vt.dirty[y]||(mode==1&&p==focused())||mode==3){
        unsigned line=(mode==1||mode==3)&&p==focused()?copy_top+y:p->count+y;
        unsigned low=copy_mark<copy_line?copy_mark:copy_line,high=copy_mark>copy_line?copy_mark:copy_line;
        for(x=0;x<p->w;x++){
            struct vt_cell c=line_cell(p,line,x);
            if((mode==1||mode==3)&&p==focused()&&((mark_set&&line>=low&&line<=high)||line==copy_line))c.colour=(c.colour&0xff00)|0x70;
            p->committed[y*p->w+x]=c;
        }
        p->vt.dirty[y]=0;
    }
}
static unsigned desktop_tasks(void){unsigned i,n=0;for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==current)n++;return n;}
static int desktop_task(int x){
    unsigned i,n=desktop_tasks(),at=0;
    if(x<0||x>=(int)cols||!n)return -1;
    for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==current){
        if((unsigned)x>=at*cols/n&&(unsigned)x<(at+1)*cols/n)return i;at++;
    }
    return -1;
}
static void compose_desktop(void){
    unsigned x,y,i,z,n=desktop_tasks(),at=0;char title[80];
    for(y=1;y<rows-1;y++)fill_row(y,0x08);
    for(z=0;z<PANES;z++)for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].shown&&!panes[i].minimized&&panes[i].z==z){
        struct pane *p=&panes[i];unsigned colour=tabs[current].focus==(int)i?0x0f:0x08;
        /* Light gray title strips with a quiet blue accent; frame sides/bottom
         * retain the desktop background. ANSI grays work on every terminal. */
        unsigned header=tabs[current].focus==(int)i?0x74:0x78;
        commit_pane(p);
        if(p->framed){
            for(x=p->x-1;x<=p->x+p->w;x++){put(x,p->y-1,' ',header);put(x,p->y+p->h,0x2500,colour);}
            for(y=p->y;y<p->y+p->h;y++){put(p->x-1,y,0x2502,colour);put(p->x+p->w,y,0x2502,colour);}
            put(p->x-1,p->y-1,0x250c,header);put(p->x+p->w,p->y-1,0x2510,header);
            put(p->x-1,p->y+p->h,0x2514,colour);put(p->x+p->w,p->y+p->h,0x2518,colour);
            snprintf(title,sizeof(title),"%u %s",i+1,p->title);
            label(p->x,p->y-1,title,header,p->x+p->w-9);
            label(p->x+p->w-9,p->y-1,tabs[current].zoom?"[_][-][x]":"[_][+][x]",header,p->x+p->w);
        }
        for(y=0;y<p->h;y++)for(x=0;x<p->w;x++){
            struct vt_cell c=p->committed&&x<p->committed_w&&y<p->committed_h?p->committed[y*p->committed_w+x]:blank_cell;
            put(p->x+x,p->y+y,c.ch,c.colour);
        }
    }
    /* A terminal cannot show half a wide grapheme at an occlusion boundary. */
    for(y=1;y<rows-2;y++)for(x=0;x<cols;x++){
        uint32_t ch=screen[y*cols+x].ch;
        if(ch==RU_CONTINUATION){if(!x||ru_glyph_width(screen[y*cols+x-1].ch)!=2)put(x,y,' ',screen[y*cols+x].colour);}
        else if(ru_glyph_width(ch)==2&&(x+1==cols||screen[y*cols+x+1].ch!=RU_CONTINUATION))put(x,y,' ',screen[y*cols+x].colour);
    }
    if(cols>=MIN_COLS+2&&rows>=MIN_ROWS+5){
        fill_row(rows-2,0x87);
        for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==current){
            unsigned end=(at+1)*cols/n,colour=panes[i].minimized?0x87:tabs[current].focus==(int)i?0x70:0x8f;
            for(x=at*cols/n;x<end;x++)put(x,rows-2,' ',colour);
            snprintf(title,sizeof(title)," %u:%s ",i+1,panes[i].title);label(at*cols/n,rows-2,title,colour,end);at++;
        }
    }
}
