#include <time.h>
#include <unistd.h>
#include <errno.h>

/* Divide *tp by 10 and also return the remainder */
/* On processors with real idiv we should probably just use that */
static unsigned long div10quickm(unsigned long *tp)
{
	unsigned long q, r, t;
	t = *tp;
	q = (t >> 1) + (t >> 2);
	q = q + (q >> 4);
	q = q + (q >> 8);
	q = q + (q >> 16);
	q >>= 3;
	r = t - (((q << 2) + q) << 1);
	q += (r + 6) >> 4;
	*tp = q;
	return t - (((q << 2) + q) << 1);
}

int clock_gettime(clockid_t clk_id, struct timespec *res)
{
  int r;
  unsigned long d;
  __ktime_t tmp;

  res->tv_nsec = 0;
  switch(clk_id) {
  case CLOCK_REALTIME:
    if (_time(&tmp, 0) < 0) return -1;
    res->tv_sec = tmp.low;
#ifndef NO_64BIT
    res->tv_sec |= ((uint64_t)tmp.high) << 32;
#endif
    return 0;
  case CLOCK_MONOTONIC:
    if (_time(&tmp, 1) < 0) return -1;
    res->tv_sec = tmp.low;
    d = res->tv_sec;
    /* We know that this wraps at 2^32 ticks which also means we know
       it'll fit 32bits */
    r = div10quickm(&d);
    /* The counter is in deciseconds, so the remainder is in units of
       100000000ns. The old factor of 100000 made all sub-second time
       vanish. */
    res->tv_nsec = 100000000UL * r;
    res->tv_sec = d;
    return 0;
  default:
    errno = EINVAL;
  }
  return -1;
}