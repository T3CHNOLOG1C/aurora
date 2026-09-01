/*
 * OpenMelee local patch (see OSAlloc.cpp): `OSAllocFromHeap`/`OSFreeToHeap`
 * are implemented natively in OpenMelee/src/os_alloc_compat.c (cross-thread
 * locking around this heap, which has no locking of its own), which
 * forwards to the real implementations under these names. Needs to become
 * an upstream Aurora PR (or a local override header, matching the
 * project's other local Aurora patches) before this repo is real -- see
 * pc_port.md.
 */
#ifndef _DOLPHIN_OS_ALLOC_REAL_H_
#define _DOLPHIN_OS_ALLOC_REAL_H_

#include <dolphin/os/OSAlloc.h>

#ifdef __cplusplus
extern "C" {
#endif

void* Aurora_OSAllocFromHeap_Real(OSHeapHandle heap, u32 size);
void Aurora_OSFreeToHeap_Real(OSHeapHandle heap, void* ptr);

/* OpenMelee debug aid: dump the free/allocated lists straight to stderr
 * (OSDumpHeap goes through Aurora's log filter, which is usually off) and
 * report the arena bounds, so callers can tell "inside the heap" from
 * "elsewhere in MEM1". */
void Aurora_OSDumpHeapRaw(OSHeapHandle heap);
void Aurora_OSGetArenaBounds(void** start, void** end);

#ifdef __cplusplus
}
#endif

#endif
