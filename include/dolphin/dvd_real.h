/*
 * melee-pc local patch (see dvd.cpp): `DVDReadAsyncPrio` itself is
 * implemented natively in melee-pc/src/dvd_compat.c (byte-swap handling for
 * big-endian disc data), which forwards to the real implementation under
 * this name. Needs to become an upstream Aurora PR (or a local override
 * header, matching the project's other local Aurora patches) before this
 * repo is real -- see pc_port.md.
 */
#ifndef _DOLPHIN_DVD_REAL_H_
#define _DOLPHIN_DVD_REAL_H_

#include <dolphin/dvd.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL Aurora_DVDReadAsyncPrio_Real(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                                  DVDCallback callback, s32 prio);

#ifdef __cplusplus
}
#endif

#endif
