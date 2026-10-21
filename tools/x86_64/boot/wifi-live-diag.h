/* Bounded per-association telemetry, not connection/rekey policy. Times are
 * calibrated native clock microseconds (the live poll's now argument), never
 * wall time or firmware beacon intervals. RX times mean host processing, not
 * over-the-air arrival: delayed RX may look fresh when a stalled poll resumes.
 * Interpret age alongside poll gaps and raw firmware missed-beacon counts.
 * No I/O or allocation in helpers. */
#ifndef RIPLIX_WIFI_LIVE_DIAG_H
#define RIPLIX_WIFI_LIVE_DIAG_H
#include <stdint.h>
enum {
    WIFI_LIVE_DIAG_POLL=1, WIFI_LIVE_DIAG_AP_RX=2,
    WIFI_LIVE_DIAG_DATA_RX=4, WIFI_LIVE_DIAG_MISSED=8
};
typedef struct {
    uint64_t first_poll_us,last_poll_us,poll_gap_us,max_poll_gap_us;
    uint64_t last_ap_rx_us,last_data_rx_us,missed_at_us,failed_at_us;
    uint32_t polls,ap_frames,data_frames,missed_notifications,clock_backwards;
    uint32_t missed_since_rx,missed_consecutive,missed_expected,missed_received;
    unsigned channel,seen,failed;
} WifiLiveDiag;
static inline void wifi_live_diag_count(uint32_t *n) { if(*n!=UINT32_MAX)++*n; }
/* Modulo wrap is accepted for intervals below half the u64 clock range.
 * A backwards/reset clock is recorded and produces zero diagnostic elapsed
 * time, rather than an enormous unsigned age. No watchdog uses this helper. */
static inline uint64_t wifi_live_diag_elapsed(uint64_t now,uint64_t before) {
    uint64_t delta=now-before;return delta<=UINT64_MAX/2?delta:0;
}
static inline void wifi_live_diag_init(WifiLiveDiag *d,unsigned channel) {
    *d=(WifiLiveDiag){.channel=channel};
}
static inline void wifi_live_diag_poll(WifiLiveDiag *d,uint64_t now) {
    if(d->failed)return;
    if(d->seen&WIFI_LIVE_DIAG_POLL) {
        if(now-d->last_poll_us>UINT64_MAX/2)wifi_live_diag_count(&d->clock_backwards);
        d->poll_gap_us=wifi_live_diag_elapsed(now,d->last_poll_us);
        if(d->poll_gap_us>d->max_poll_gap_us)d->max_poll_gap_us=d->poll_gap_us;
    } else { d->first_poll_us=now;d->seen|=WIFI_LIVE_DIAG_POLL; }
    d->last_poll_us=now;wifi_live_diag_count(&d->polls);
}
static inline void wifi_live_diag_ap_rx(WifiLiveDiag *d,uint64_t now) {
    if(d->failed)return;
    d->last_ap_rx_us=now;d->seen|=WIFI_LIVE_DIAG_AP_RX;wifi_live_diag_count(&d->ap_frames);
}
static inline void wifi_live_diag_data_rx(WifiLiveDiag *d,uint64_t now) {
    if(d->failed)return;
    d->last_data_rx_us=now;d->seen|=WIFI_LIVE_DIAG_DATA_RX;wifi_live_diag_count(&d->data_frames);
}
static inline void wifi_live_diag_missed(WifiLiveDiag *d,uint64_t now,uint32_t since_rx,
                                       uint32_t consecutive,uint32_t expected,uint32_t received) {
    if(d->failed)return;
    d->missed_at_us=now;d->seen|=WIFI_LIVE_DIAG_MISSED;
    wifi_live_diag_count(&d->missed_notifications);
    /* Firmware values are raw snapshots, not monotonic host counters. They
     * may wrap or reset; do not subtract them or infer packet loss totals. */
    d->missed_since_rx=since_rx;d->missed_consecutive=consecutive;
    d->missed_expected=expected;d->missed_received=received;
}
static inline void wifi_live_diag_fail(WifiLiveDiag *d,uint64_t now) {
    if(!d->failed){d->failed_at_us=now;d->failed=1;}
}
static inline uint64_t wifi_live_diag_ap_age(const WifiLiveDiag *d) {
    if(!(d->seen&WIFI_LIVE_DIAG_AP_RX))return UINT64_MAX;
    uint64_t age=(d->failed?d->failed_at_us:d->last_poll_us)-d->last_ap_rx_us;
    return age<=UINT64_MAX/2?age:UINT64_MAX;
}
static inline uint64_t wifi_live_diag_data_age(const WifiLiveDiag *d) {
    if(!(d->seen&WIFI_LIVE_DIAG_DATA_RX))return UINT64_MAX;
    uint64_t age=(d->failed?d->failed_at_us:d->last_poll_us)-d->last_data_rx_us;
    return age<=UINT64_MAX/2?age:UINT64_MAX;
}
typedef struct { char *out;unsigned capacity,used; } WifiLiveDiagText;
static inline void wifi_live_diag_char(WifiLiveDiagText *t,char c) {
    if(t->capacity && t->used<t->capacity-1)t->out[t->used]=c;
    ++t->used;
}
static inline void wifi_live_diag_text(WifiLiveDiagText *t,const char *s) {
    while(*s)wifi_live_diag_char(t,*s++);
}
static inline void wifi_live_diag_decimal(WifiLiveDiagText *t,uint64_t value) {
    char digits[20];unsigned n=0;
    do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);
    while(n)wifi_live_diag_char(t,digits[--n]);
}
/* Cause first, compact context after it. A concise caller label such as
 * "beacon loss" fits ordinary values in the existing 64-octet status field.
 * gap is the spacing between the last two polls; age is since the last host-
 * processed selected-AP frame. Both are FLOOR milliseconds. age=? means none
 * or an invalid/backwards age. Raw timestamps and seen bits retain context.
 * Returns the full required length excluding NUL; capacity0 accepts NULL.
 * Every nonzero-capacity output is terminated. A trailing ~ marks truncation.
 * This never changes the source cause/metadata or replaces a retained error. */
static inline unsigned wifi_live_diag_summary(const WifiLiveDiag *d,const char *cause,
                                             char *out,unsigned capacity) {
    WifiLiveDiagText t={out,capacity,0};
    wifi_live_diag_text(&t,cause?cause:"link stopped");
    wifi_live_diag_text(&t," ch");wifi_live_diag_decimal(&t,d->channel);
    if(d->seen&WIFI_LIVE_DIAG_MISSED) {
        wifi_live_diag_text(&t," m");wifi_live_diag_decimal(&t,d->missed_consecutive);
        wifi_live_diag_text(&t,"/r");wifi_live_diag_decimal(&t,d->missed_since_rx);
    }
    wifi_live_diag_text(&t," gap");wifi_live_diag_decimal(&t,d->poll_gap_us/1000);
    wifi_live_diag_text(&t,"ms age");
    uint64_t age=wifi_live_diag_ap_age(d);
    if(age==UINT64_MAX)wifi_live_diag_char(&t,'?');
    else {wifi_live_diag_decimal(&t,age/1000);wifi_live_diag_text(&t,"ms");}
    if(capacity) {
        unsigned n=t.used<capacity?t.used:capacity-1;
        if(t.used>=capacity && capacity>1)out[n-1]='~';
        out[n]=0;
    }
    return t.used;
}
#endif
