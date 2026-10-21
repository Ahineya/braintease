/* Native WPA2-PSK crypto boundary; no C16 pointers, allocation or host calls. */
#ifndef RIPLIX_WIFI_CRYPTO_H
#define RIPLIX_WIFI_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
void wifi_wipe(void *,size_t);
int wifi_equal(const uint8_t *,const uint8_t *,size_t);
void wifi_sha1(const uint8_t *,size_t,uint8_t[20]);
void wifi_hmac_sha1(const uint8_t *,size_t,const uint8_t *,size_t,uint8_t[20]);
int wifi_pmk(const uint8_t *,unsigned,const uint8_t *,unsigned,uint8_t[32]);
void wifi_ptk(const uint8_t[32],const uint8_t[6],const uint8_t[6],const uint8_t[32],const uint8_t[32],uint8_t[48]);
/* RFC3394,16-byte KEK,24..512 wrapped bytes; output untouched on failure. */
int wifi_unwrap(const uint8_t[16],const uint8_t *,unsigned,uint8_t *);
#endif
