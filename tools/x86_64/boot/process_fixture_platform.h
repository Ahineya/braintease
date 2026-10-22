#ifndef RIPLIX_PROCESS_FIXTURE_PLATFORM_H
#define RIPLIX_PROCESS_FIXTURE_PLATFORM_H
#include <stdint.h>
/* Acceptance-only wrapper. sentinel() must be stable during the synchronous
 * fixture and cover the caller's live runtime/shared/process state. */
void process_fixture_platform_run(uint64_t (*sentinel)(void *), void *opaque);
#endif
