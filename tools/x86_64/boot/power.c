/* ACPI FADT RESET_REG/RESET_VALUE, SystemIO byte subset only. Unsupported
 * descriptors stay unavailable; never guess an HP/QEMU reset port/value. */
#include "power.h"
PowerReset power_reset_parse(const uint8_t *p,size_t n) {
    PowerReset none={0};uint8_t sum=0;
    if(n<129 || p[0]!='F' || p[1]!='A' || p[2]!='C' || p[3]!='P')return none;
    uint32_t length=(uint32_t)p[4]|(uint32_t)p[5]<<8|(uint32_t)p[6]<<16|(uint32_t)p[7]<<24;
    if(length!=n)return none;
    for(size_t i=0;i<n;i++)sum+=p[i];
    if(sum || !(p[113]&4) || p[116]!=1 || p[117]!=8 || p[118] || p[119]>1)return none;
    for(unsigned i=122;i<128;i++)if(p[i])return none;
    unsigned port=p[120]|(unsigned)p[121]<<8;
    return port?(PowerReset){port,p[128],1}:none;
}
