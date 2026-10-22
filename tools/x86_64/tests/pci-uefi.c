/* Fake UEFI protocols only; no firmware, PCI hardware or device access. */
#include "../boot/pci-uefi.c"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
static uint8_t root_data[46*4+2],bar_data[48];
static RootIo root;
static PciIo target;
static uint32_t config[16];
static unsigned frees,bar_allocations,bar_frees,no_pci,huge_handles,stale_handle,failure_pointer,config_failure;
static unsigned no_bar,handle_failure;
static Status bar_error;
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
static void descriptor(uint8_t *p,unsigned type,uint64_t min,uint64_t len,unsigned width){
 memset(p,0,46);p[0]=0x8a;p[1]=0x2b;p[3]=type;put64(p+6,width);put64(p+14,min);put64(p+22,min+len-1);put64(p+38,len);
}
static Status root_config(RootIo *self,void **out){assert(self==&root);*out=root_data;return 0;}
static Status pci_read(PciIo *self,uint32_t width,uint32_t offset,size_t n,void *out){
 assert(self==&target && width==2 && n==1 && offset<=60 && !(offset&3));if(config_failure && offset==4)return EFI_ERROR(7);
 if(stale_handle){assert(offset==0);*(uint32_t *)out=0xffffffff;return 0;}
 *(uint32_t *)out=config[offset/4];return 0;
}
static Status root_read(RootIo *self,uint32_t width,uint64_t address,size_t n,void *out){
 assert(self==&root && width==2 && n==1);
 unsigned bus=(unsigned)(address>>24)&255,dev=(unsigned)(address>>16)&255,fn=(unsigned)(address>>8)&255,offset=(unsigned)address&255;
 assert(dev<32 && fn<8 && offset<=60 && !(offset&3));
 *(uint32_t *)out=bus==0 && (dev*8+fn)==TARGET_BDF?config[offset/4]:0xffffffff;return 0;
}
static Status location(PciIo *self,size_t *seg,size_t *bus,size_t *dev,size_t *fn){
 assert(self==&target);*seg=0;*bus=0;*dev=0x15;*fn=2;return 0;
}
static Status bars(PciIo *self,uint8_t index,uint64_t *supports,void **out){
 assert(self==&target && !supports);*out=0;
 if(index || no_bar){if(failure_pointer)*out=(void *)(uintptr_t)1;return bar_error?bar_error:EFI_UNSUPPORTED;}
 *out=malloc(sizeof(bar_data));assert(*out);memcpy(*out,bar_data,sizeof(bar_data));++bar_allocations;return 0;
}
static Status free_pool(void *p){
 assert(p && p!=root_data && p!=bar_data);++frees;
 /* BAR descriptor buffers begin with the resource tag, arrays with handles. */
 if(*(uint8_t *)p==0x8a)++bar_frees;
 free(p);return 0;
}
static int guid_equal(const Guid *a,const Guid *b){return !memcmp(a,b,sizeof(*a));}
static Status handles(uint32_t type,const Guid *guid,void *search,size_t *count,Handle **out){
 assert(type==2 && !search);
 if(handle_failure || (no_pci==2 && guid_equal(guid,&pci_guid))){
  *out=(void *)(uintptr_t)1;*count=123;return handle_failure?EFI_ERROR(7):EFI_NOT_FOUND;
 }
 *out=malloc(sizeof(Handle));assert(*out);
 if(guid_equal(guid,&root_guid)){*count=huge_handles?257:1;(*out)[0]=&root;}
 else {assert(guid_equal(guid,&pci_guid));*count=no_pci?0:1;(*out)[0]=&target;}return 0;
}
static Status protocol(Handle handle,const Guid *guid,void **out){
 if(guid_equal(guid,&root_guid)){assert(handle==&root);*out=&root;}
 else{assert(guid_equal(guid,&pci_guid) && handle==&target);*out=&target;}return 0;
}
static void reset(void){
 memset(&root,0,sizeof(root));memset(&target,0,sizeof(target));memset(config,0,sizeof(config));memset(root_data,0,sizeof(root_data));
 root.configuration=root_config;root.pci.read=root_read;root.segment=0;
 target.get_location=location;target.get_bar_attributes=bars;target.pci.read=pci_read;
 descriptor(root_data,0,0x4800000000ULL,0x100000000ULL,64);
 descriptor(root_data+46,2,0,256,0);root_data[92]=0x79;root_data[93]=0;
 descriptor(bar_data,0,0x48192ce000ULL,4096,64);bar_data[46]=0x79;bar_data[47]=0;
 config[0]=0x7e7a8086;config[4]=4;
 frees=bar_allocations=bar_frees=no_pci=huge_handles=stale_handle=failure_pointer=config_failure=0;
 no_bar=handle_failure=0;
 bar_error=0;
}
int main(void){
 BootServices s={0};s.locate_handle_buffer=handles;s.handle_protocol=protocol;s.free_pool=free_pool;
 BootInfo b;unsigned groups=0;
 reset();pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.window_count==1 && b.pci.span_count==1);
 assert(b.pci.target_bar0_base==0x48192ce000ULL && b.pci.target_bar0_length==4096);
 assert(b.pci.windows[0].bus_first==0 && b.pci.windows[0].bus_last==255 && !b.pci.windows[0].prefetch);
 assert(frees==3 && bar_allocations==bar_frees);++groups;
 reset();config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE);++groups;
 reset();config[4]=0x192cf004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==2);
 assert(b.pci.occupied[1].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[1].base==0x48192cf000ULL &&
        b.pci.occupied[1].length==0x1000);++groups;
 reset();no_pci=1;config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base);
 assert(b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0x48192ce000ULL &&
        b.pci.occupied[0].length==0x2000);++groups;
 reset();put64(root_data+30,4096);pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"translation"));++groups;
 reset();root_data[1]=0x2a;pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"descriptor"));++groups;
 reset();put64(root_data+14,UINT64_MAX-100);pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"bounds"));++groups;
 reset();put64(bar_data+30,1);pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_INCOMPLETE && bar_allocations==bar_frees && frees==3);++groups;
 reset();huge_handles=1;pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && frees==1);++groups;
 reset();root_data[46]=0x79;pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"bus range"));++groups;
 reset();config[12]=0xc0000000;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==2);
 assert(b.pci.occupied[0].kind==PCI_BOOT_ROM32 && b.pci.occupied[0].base==1 && b.pci.occupied[0].length==UINT32_MAX);++groups;
 reset();config[3]=1u<<16;config[8]=0x10001000;config[9]=0x0000fff0;config[14]=0xc0000000;
 pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==3);++groups;
 reset();put64(bar_data+22,4095);pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.target_bar0_base==0x48192ce000ULL);++groups;
 reset();put64(bar_data+22,0);pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE);++groups;
 reset();put64(bar_data+22,UINT64_MAX);pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE);++groups;
 reset();put64(root_data+22,0);pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"ROOT t=00") && strstr(b.pci.reason,"max=0000000000000000"));++groups;
 reset();put64(bar_data+14,UINT64_MAX-100);put64(bar_data+22,0);pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"BAR t=00") && strstr(b.pci.reason,"len=0000000000001000"));++groups;
 reset();stale_handle=1;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && b.pci.target_bar0_base==0x48192ce000ULL);
 assert(frees==3 && bar_allocations==bar_frees);++groups;
 reset();stale_handle=1;failure_pointer=1;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && frees==3 && bar_allocations==bar_frees);++groups;
 reset();config_failure=1;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"bdf=00aa"));++groups;
 reset();no_pci=1;config[4]=0xe0123000;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base);
 assert(b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0xe0123000ULL &&
        b.pci.occupied[0].length==0x1000);++groups;
 reset();no_pci=1;config[4]=0x10000004;config[5]=0;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base);
 assert(b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0x10000000ULL &&
        b.pci.occupied[0].length==0x10000000ULL);++groups;
 reset();no_pci=1;config[4]=4;config[5]=0x80000000;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1);
 assert(b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0x8000000000000000ULL &&
        b.pci.occupied[0].length==0x7fffffffffffffffULL);++groups;
 /* Live assigned BAR + EFI_UNSUPPORTED must take the same conservative path
  * as absent optional handles, without touching a failed output pointer. */
 reset();no_bar=1;failure_pointer=1;config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && !b.pci.target_bar0_base && frees==2);
 assert(b.pci.span_count==1 && b.pci.occupied[0].base==0x48192ce000ULL && b.pci.occupied[0].length==0x2000);++groups;
 reset();no_bar=1;bar_error=EFI_ERROR(9);failure_pointer=1;config[4]=0x192ce004;config[5]=0x48;
 pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && frees==2);
 assert(b.pci.occupied[0].base==0x48192ce000ULL && b.pci.occupied[0].length==0x2000);++groups;
 /* Stale optional handles do not veto the independent mandatory root-bus
  * inventory, whatever status their cached metadata query returns. */
 reset();stale_handle=1;no_bar=1;bar_error=EFI_ERROR(7);failure_pointer=1;
 config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && frees==2 && !b.pci.target_bar0_base);
 assert(b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0x48192ce000ULL &&
        b.pci.occupied[0].length==0x2000);++groups;
 reset();stale_handle=1;no_bar=1;bar_error=EFI_ERROR(2);failure_pointer=1;
 config[3]=1u<<16;config[4]=0;config[8]=0x0000fff0;
 config[9]=0x00010001;config[10]=0x48;config[11]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && frees==2);
 assert(b.pci.occupied[0].kind==PCI_BOOT_BRIDGE && b.pci.occupied[0].base==0x4800000000ULL &&
        b.pci.occupied[0].length==0x100000);++groups;
 reset();stale_handle=1;no_bar=1;bar_error=EFI_ERROR(7);config[3]=1u<<16;config[2]=0x06040100;
 pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"subtractive bridge") &&
        strstr(b.pci.reason,"bdf=00aa"));++groups;
 /* Successful optional metadata is not necessarily an actual PCI decoder:
  * power-of-two size/alignment are required before granting exact/cache use. */
 reset();descriptor(bar_data,0,0x192cc000,0xe6d34000,32);
 config[4]=0x192cc004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base &&
        b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].base==0x48192cc000ULL &&
        b.pci.occupied[0].length==0x4000 && bar_allocations==bar_frees);++groups;
 reset();descriptor(bar_data,0,0x48192ce000ULL,0x3000,64);
 config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base &&
        b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].length==0x2000);++groups;
 reset();descriptor(bar_data,0,0x48192ce000ULL,0x4000,64);
 config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base &&
        b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && b.pci.occupied[0].length==0x2000);++groups;
 reset();stale_handle=1;descriptor(bar_data,0,0x48192ce000ULL,0x3000,64);
 config[4]=0x192ce004;config[5]=0x48;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1 && !b.pci.target_bar0_base &&
        b.pci.occupied[0].kind==PCI_BOOT_UNKNOWN && bar_allocations==bar_frees);++groups;
 reset();no_pci=2;config[4]=0xe0123000;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && frees==1 && b.pci.span_count==1);++groups;
 reset();handle_failure=1;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_INCOMPLETE && !frees);++groups;
 reset();no_bar=1;config[4]=0xa0002;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==1);
 assert(b.pci.occupied[0].base==0xa0000 && b.pci.occupied[0].length==0x20000);++groups;
 reset();no_bar=1;config[3]=2u<<16;config[4]=0xe0000000;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.span_count==2);
 assert(b.pci.occupied[0].base==1 && b.pci.occupied[0].length==UINT32_MAX);++groups;
 reset();config[3]=1u<<16;config[8]=0x100f100f;config[9]=0x0000fff0;pci_uefi_capture(&s,&b);
 assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.occupied[0].kind==PCI_BOOT_BRIDGE &&
        b.pci.occupied[0].base==0x10000000 && b.pci.occupied[0].length==0x100000);++groups;
 reset();descriptor(root_data+46,2,0,128,0);descriptor(root_data+92,2,128,128,0);root_data[138]=0x79;
 pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_COMPLETE && b.pci.window_count==2);
 assert(b.pci.windows[0].bus_first==0 && b.pci.windows[0].bus_last==127 &&
        b.pci.windows[1].bus_first==128 && b.pci.windows[1].bus_last==255);++groups;
 reset();descriptor(root_data+46,2,0,129,0);descriptor(root_data+92,2,128,128,0);root_data[138]=0x79;
 pci_uefi_capture(&s,&b);assert(b.pci.status==PCI_BOOT_INCOMPLETE && strstr(b.pci.reason,"overlapping"));++groups;
 printf("PCI_UEFI_CAPTURE_OK groups=%u borrowed_root_not_freed=1 caller_buffers_freed=1 no_PCI_writes=1\n",groups);
 return 0;
}
