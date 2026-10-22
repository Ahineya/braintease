/* HID-over-I2C wire ordering follows Linux i2c-hid-core.c and OpenBSD ihidev.c.
 * Descriptor parsing is bounded and native-byte based. No heap or IRQ writes. */
#include "hid_i2c.h"
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static uint32_t uvalue(const uint8_t *p,unsigned n){
    uint32_t x=0;for(unsigned i=0;i<n;i++)x|=(uint32_t)p[i]<<(i*8);return x;
}
static int32_t svalue(uint32_t v,unsigned bits){
    if(!bits)return 0;
    if(bits<32 && (v&(1u<<(bits-1))))v|=~((1u<<bits)-1);
    return (int32_t)v;
}
typedef struct {uint32_t page,size,count,id;int32_t min,max;} Global;
const char *hid_i2c_parse_descriptor(HidI2cLayout *l,const uint8_t *p,size_t n){
    if(!l || !p || !n || n>HID_I2C_MAX_DESCRIPTOR)return "HID descriptor bounds";
    *l=(HidI2cLayout){0};
    Global g={0},stack[4];unsigned sp=0;
    uint32_t usages[32],umin=0,umax=0;unsigned uc=0,range=0;
    uint8_t collection[16];unsigned depth=0,finger=0,nextfinger=0;
    for(size_t at=0;at<n;){
        uint8_t b=p[at++];
        if(b==0xfe)return "HID long item unsupported";
        unsigned bytes=b&3;if(bytes==3)bytes=4;
        if(bytes>n-at)return "HID truncated item";
        uint32_t v=uvalue(p+at,bytes);at+=bytes;
        unsigned type=(b>>2)&3,tag=b>>4;
        if(type==1){
            switch(tag){
            case 0:g.page=v;break;
            case 1:g.min=svalue(v,bytes*8);break;
            case 2:g.max=g.min<0?svalue(v,bytes*8):(int32_t)v;break;
            case 7:g.size=v;break;
            case 8:if(!v || v>255)return "HID report ID bounds";g.id=v;l->numbered=1;break;
            case 9:g.count=v;break;
            case 10:if(sp==4)return "HID global stack overflow";stack[sp++]=g;break;
            case 11:if(!sp)return "HID global stack underflow";g=stack[--sp];break;
            default:break;
            }
        }else if(type==2){
            if(tag==0){if(uc==32)return "HID usage list overflow";usages[uc++]=v;}
            else if(tag==1){umin=v;range|=1;}
            else if(tag==2){umax=v;range|=2;}
        }else if(type==0){
            if(tag==10){
                if(depth==16)return "HID collection overflow";
                collection[depth++]=(uint8_t)finger;
                uint32_t usage=uc?usages[0]:0;
                if(g.page==0x0d && usage==0x22){
                    if(nextfinger==255)return "HID contact collection bounds";
                    finger=++nextfinger;
                }
            }else if(tag==12){if(!depth)return "HID collection underflow";finger=collection[--depth];}
            else if(tag==8 || tag==9 || tag==11){
                unsigned rt=tag==8?0:tag==9?1:2;
                if(g.size>32 || g.count>4096 || (g.count && g.size>4096/g.count))
                    return "HID report field bounds";
                unsigned total=g.size*g.count,offset=l->report_bits[rt][g.id];
                if(total>4096-offset)return "HID report length bounds";
                l->report_bits[rt][g.id]=(uint16_t)(offset+total);
                if(!(v&1) && (v&2) && g.size){ /* data, variable */
                    for(unsigned j=0;j<g.count;j++){
                        uint32_t usage=0;
                        if(uc)usage=usages[j<uc?j:uc-1];
                        else if(range==3 && umax>=umin)usage=umin+(j<=umax-umin?j:umax-umin);
                        uint32_t page=usage>>16?usage>>16:g.page;
                        usage&=0xffff;
                        if(!((page==1 && (usage==0x30 || usage==0x31 || usage==0x38)) ||
                             (page==9 && usage>=1 && usage<=5) ||
                             (page==0x0d && (usage==0x42 || usage==0x51 || usage==0x52 || usage==0x54 || usage==0x57 || usage==0x58 || usage==0x60)))){
                            if(rt==2)l->feature_unknown[g.id]=1;
                            continue;
                        }
                        if(rt==2 && !(page==0x0d && (usage==0x52 || usage==0x57 || usage==0x58 || usage==0x60)))
                            l->feature_unknown[g.id]=1;
                        if(l->count==HID_I2C_MAX_FIELDS)return "HID pointer field overflow";
                        l->fields[l->count++]=(HidI2cField){(uint16_t)page,(uint16_t)usage,
                            (uint16_t)(offset+j*g.size),(uint16_t)g.size,g.min,g.max,
                            (uint8_t)g.id,(uint8_t)rt,(uint8_t)((v>>2)&1),(uint8_t)finger};
                    }
                }else if(rt==2 && !(v&1) && total)l->feature_unknown[g.id]=1;
            }
            uc=0;range=0; /* Local state expires after every main item. */
        }
    }
    if(depth || sp)return "HID unclosed collection/global stack";
    unsigned xy=0;for(unsigned i=0;i<l->count;i++){
        const HidI2cField *f=&l->fields[i];if(!f->type && f->page==1 && (f->usage==0x30||f->usage==0x31))xy++;
    }
    return xy>=2?0:"HID pointer axes missing";
}
static int32_t extract(const uint8_t *p,const HidI2cField *f){
    uint32_t v=0;for(unsigned i=0;i<f->size;i++)v|=(uint32_t)((p[(f->offset+i)/8]>>((f->offset+i)%8))&1)<<i;
    return f->minimum<0?svalue(v,f->size):(int32_t)v;
}
int hid_i2c_decode(const HidI2cLayout *l,const uint8_t *p,size_t n,HidI2cPointer *out){
    if(!l || !p || !out || !n)return -1;
    unsigned id=0;if(l->numbered){id=*p++;n--;}
    unsigned need=(l->report_bits[0][id]+7)/8;
    if(!need)return 0;
    if(n<need)return -1;
    /* Prefer the first active Finger collection, not a later inactive slot. */
    unsigned selected=0,has_tip=0;
    for(unsigned i=0;i<l->count;i++){
        const HidI2cField *f=&l->fields[i];
        if(!f->type && f->report_id==id && f->page==0x0d && f->usage==0x42){
            has_tip=1;if(extract(p,f) && !selected)selected=f->finger;
        }
    }
    HidI2cPointer e={0};unsigned axes=0,x_absolute=0,y_absolute=0;
    e.contact=!has_tip;
    for(unsigned i=0;i<l->count;i++){
        const HidI2cField *f=&l->fields[i];
        if(f->type || f->report_id!=id)continue;
        int32_t v=extract(p,f);
        if(f->page==9){if(v)e.buttons|=1u<<(f->usage-1);continue;}
        if(f->page==0x0d && f->usage==0x51 && f->finger==selected)e.contact_id=(unsigned)v;
        if(f->page==0x0d && f->usage==0x42){if(f->finger==selected && v)e.contact=1;continue;}
        if(f->finger && f->finger!=selected)continue;
        if(f->page!=1)continue;
        if(f->usage==0x30 && !(axes&1)){e.x=v;e.x_min=f->minimum;e.x_max=f->maximum;axes|=1;x_absolute=!f->relative;}
        else if(f->usage==0x31 && !(axes&2)){e.y=v;e.y_min=f->minimum;e.y_max=f->maximum;axes|=2;y_absolute=!f->relative;}
        else if(f->usage==0x38)e.wheel=v;
    }
    if(axes!=3){ /* Contact lift reports still release the pointer/buttons. */
        if(has_tip && !e.contact){e.absolute=1;*out=e;return 1;}return 0;
    }
    if(x_absolute!=y_absolute)return -1;
    e.absolute=x_absolute;
    if(e.absolute && (e.x_max<=e.x_min || e.y_max<=e.y_min ||
       e.x<e.x_min || e.x>e.x_max || e.y<e.y_min || e.y>e.y_max))return -1;
    *out=e;return 1;
}
static const char *xfer(HidI2c *d,const uint8_t *w,size_t wn,uint8_t *r,size_t rn){
    return d->io.transfer(d->io.context,d->config.address,w,wn,r,rn);
}
static const char *command(HidI2c *d,uint8_t op,uint8_t arg){
    uint8_t b[4];put16(b,d->command_register);b[2]=arg;b[3]=op;return xfer(d,b,4,0,0);
}
/* Linux mt_set_modes configures InputMode plus Surface/Button switches.
 * GET preserves unrelated features. Linux does not mandate GET: if it fails,
 * a report containing only these writable fields and constant padding can be
 * safely constructed. Otherwise leave that report unchanged and record why. */
static const char *configure_features(HidI2c *d){
    unsigned first_mode=HID_I2C_MAX_FIELDS;
    for(unsigned i=0;i<d->layout.count;i++){
        const HidI2cField *f=&d->layout.fields[i];
        if(f->type==2 && f->page==0x0d && f->usage==0x52){first_mode=i;break;}
    }
    const char *first_error=0;
    for(unsigned id=0;id<256;id++){
        unsigned update=0;
        for(unsigned i=0;i<d->layout.count;i++){
            const HidI2cField *f=&d->layout.fields[i];
            if(f->type==2 && f->report_id==id && f->page==0x0d &&
               ((f->usage==0x52 && i==first_mode)||f->usage==0x57||f->usage==0x58||f->usage==0x60))update=1;
        }
        if(!update)continue;
        unsigned payload=(d->layout.report_bits[2][id]+7)/8;
        unsigned number=id?1:0,length=2+number+payload;
        uint8_t cmd[10],report[HID_I2C_MAX_INPUT]={0},output[HID_I2C_MAX_INPUT+10];
        if(length>sizeof(report)){if(!first_error)first_error="HID feature size bounds";continue;}
        put16(cmd,d->command_register);cmd[2]=(uint8_t)(0x30|(id<15?id:15));cmd[3]=2;
        unsigned cn=4;if(id>=15)cmd[cn++]=(uint8_t)id;
        put16(cmd+cn,d->data_register);cn+=2;
        const char *e=xfer(d,cmd,cn,report,length);
        if(e || le16(report)!=length || (number && report[2]!=id)){
            if(d->layout.feature_unknown[id]){
                if(!first_error)first_error=e?e:"HID feature reply";continue;
            }
            for(unsigned i=0;i<length;i++)report[i]=0;
            put16(report,(uint16_t)length);if(number)report[2]=(uint8_t)id;
        }
        uint8_t *data=report+2+number;
        for(unsigned i=0;i<d->layout.count;i++){
            const HidI2cField *f=&d->layout.fields[i];
            if(f->type!=2 || f->report_id!=id || f->page!=0x0d)continue;
            unsigned value;
            if(f->usage==0x52 && i==first_mode)value=3;
            else if(f->usage==0x57 || f->usage==0x58)value=1;
            else if(f->usage==0x60)value=0;else continue;
            if(f->minimum>(int32_t)value || f->maximum<(int32_t)value){
                if(!first_error)first_error="HID feature value unsupported";continue;
            }
            for(unsigned bit=0;bit<f->size;bit++){
                unsigned at=f->offset+bit;
                data[at/8]=(uint8_t)((data[at/8]&~(1u<<(at%8)))|(((value>>bit)&1)<<(at%8)));
            }
        }
        cmd[3]=3;
        for(unsigned i=0;i<cn;i++)output[i]=cmd[i];
        for(unsigned i=0;i<length;i++)output[cn+i]=report[i];
        e=xfer(d,output,cn+length,0,0);if(e && !first_error)first_error=e;
    }
    return first_error;
}
static const char *power_on(HidI2c *d){
    const char *e=command(d,8,0);
    if(e){d->io.delay_us(d->io.context,500);e=command(d,8,0);}
    if(!e){d->powered=1;d->io.delay_us(d->io.context,60000);}
    return e;
}
const char *hid_i2c_init(HidI2c *d,const HidI2cConfig *c,const HidI2cIo *io){
    if(!d || !c || !io || !io->transfer || !io->delay_us || c->address<8 || c->address>0x77)
        return "HID I2C resources missing";
    *d=(HidI2c){.io=*io,.config=*c};
    uint8_t reg[2],h[30],desc[HID_I2C_MAX_DESCRIPTOR];
    put16(reg,c->descriptor_register);
    const char *e=xfer(d,reg,2,h,sizeof(h));
    /* Linux address-probe wake retry, applied to our first descriptor read. */
    if(e){io->delay_us(io->context,500);e=xfer(d,reg,2,h,sizeof(h));}
    if(e)return e;
    if(le16(h)!=30 || le16(h+2)!=0x100)return "HID I2C descriptor version/length";
    size_t dn=le16(h+4);
    if(!dn || dn>sizeof(desc) || le16(h+10)<3 || le16(h+10)>HID_I2C_MAX_INPUT)
        return "HID I2C report size bounds";
    if((c->vendor && le16(h+20)!=c->vendor) || (c->product && le16(h+22)!=c->product))
        return "HID I2C device identity";
    d->report_register=le16(h+6);d->input_register=le16(h+8);d->max_input=le16(h+10);
    d->command_register=le16(h+16);d->data_register=le16(h+18);
    /* Linux retries failed reset commands; OpenBSD cold boot polls a two-byte
     * zero ACK for up to five seconds. ACK timeout is diagnostic (Linux warns
     * then continues), not proof that a responsive HID device is unusable. */
    for(unsigned attempt=0;attempt<3;attempt++){
        e=power_on(d);if(!e)e=command(d,1,0);
        if(!e)break;
        (void)command(d,8,1);io->delay_us(io->context,100000);
    }
    if(e)return e;
    unsigned reset=0;
    for(unsigned wait=0;wait<50;wait++){
        io->delay_us(io->context,100000);
        e=xfer(d,0,0,d->input,2);
        if(!e && !le16(d->input)){reset=1;break;}
    }
    if(reset)d->resets++;else d->reset_timeouts++;
    e=power_on(d);if(e)return e;
    put16(reg,d->report_register);e=xfer(d,reg,2,desc,dn);if(e)return e;
    e=hid_i2c_parse_descriptor(&d->layout,desc,dn);if(e)return e;
    d->feature_error=configure_features(d);
    if(d->feature_error)d->feature_errors++;
    d->ready=1;return 0;
}
static void release_pointer(HidI2c *d,HidI2cEmit emit,void *context){
    if(d->have_pointer && (d->last_pointer.contact || d->last_pointer.buttons)){
        HidI2cPointer release=d->last_pointer;release.contact=0;release.buttons=0;
        release.x=release.y=release.wheel=0;release.absolute=1;
        emit(context,&release);d->last_pointer=release;
    }
}
const char *hid_i2c_poll(HidI2c *d,HidI2cEmit emit,void *context){
    if(!d || !d->ready || !emit)return "HID I2C not initialized";
    if(d->retry_polls){d->retry_polls--;return 0;}
    int pending=d->io.pending?d->io.pending(d->io.context):1;
    if(pending<0)return "HID I2C GPIO unavailable"; /* Resource failure, not data. */
    if(!pending)return 0;
    const char *e=xfer(d,0,0,d->input,d->max_input);
    if(e){
        /* Both upstream IRQ handlers drop a failed input transaction without
         * detaching. Back off100calls (~1s at the HP adapter's100Hz), so a
         * stuck bus cannot monopolize the cooperative runtime with retries. */
        d->io_errors++;d->retry_polls=100;release_pointer(d,emit,context);
        /* Recovery policy, not a packet-validity condition: give the platform
         * a chance to reset/reinitialize a persistently unavailable transport. */
        if(++d->consecutive_errors>=3)return "HID I2C repeated transport failure";
        return 0;
    }
    d->consecutive_errors=0;
    size_t n=le16(d->input);
    if(!n){d->resets++;release_pointer(d,emit,context);return 0;}
    if(n==0xffff){release_pointer(d,emit,context);return 0;}
    if(n<2 || n>d->max_input){d->malformed++;release_pointer(d,emit,context);return 0;}
    if(n==2){release_pointer(d,emit,context);return 0;}
    HidI2cPointer event;
    int result=hid_i2c_decode(&d->layout,d->input+2,n-2,&event);
    if(result<0){d->malformed++;release_pointer(d,emit,context);return 0;}
    d->reports++;
    if(result){emit(context,&event);d->last_pointer=event;d->have_pointer=1;}
    return 0;
}
const char *hid_i2c_sleep(HidI2c *d){
    if(!d || !d->io.transfer)return "HID I2C not initialized";
    d->ready=0;if(!d->powered)return 0;
    const char *e=command(d,8,1);if(!e)d->powered=0;return e;
}
