#include "pci-resource.h"
static int power_two(uint64_t n){return n && !(n&(n-1));}
const char *pci_resource_select(const PciResourceRange *windows,size_t count,
                               const PciResourceRange *occupied,size_t used,
                               uint64_t size,uint64_t align,uint64_t limit,
                               uint64_t *address){
    if(!windows || !count || count>PCI_RESOURCE_MAX_WINDOWS ||
       (used && !occupied) || used>PCI_RESOURCE_MAX_OCCUPIED || !address ||
       !power_two(size) || !power_two(align) || size>align || !limit)
        return "PCI resource allocation bounds";
    for(size_t i=0;i<count;i++)if(windows[i].start>=windows[i].end)
        return "PCI root window malformed";
    for(size_t i=0;i<used;i++)if(occupied[i].start>=occupied[i].end)
        return "PCI occupied resource malformed";
    uint64_t best=0;unsigned found=0;
    for(size_t w=0;w<count;w++){
        uint64_t begin=windows[w].start;
        uint64_t end=windows[w].end<limit?windows[w].end:limit;
        if(end<=begin || size>end-begin)continue;
        uint64_t candidate=(end-size)&~(align-1);
        for(size_t step=0;step<=used;step++){
            if(candidate<begin || size>end-candidate)break;
            uint64_t below=UINT64_MAX;
            for(size_t i=0;i<used;i++){
                if(candidate<occupied[i].end && occupied[i].start<candidate+size &&
                   occupied[i].start<below)below=occupied[i].start;
            }
            if(below==UINT64_MAX){if(!found || candidate>best)best=candidate;found=1;break;}
            if(below<size)break;
            uint64_t next=(below-size)&~(align-1);
            if(next>=candidate)break;
            candidate=next;
        }
    }
    if(!found)return "PCI root windows have no free aligned span";
    *address=best;return 0;
}
