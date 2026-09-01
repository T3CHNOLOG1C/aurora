#pragma once

#include "../../internal.hpp"
#include <dolphin/types.h>

static aurora::Module Log("aurora::os");

constexpr uintptr_t ARENA_START_OFFSET = 0x4000;

/* extern "C": OpenMelee's compat layer references these from C
 * (src/gx_compat.c, src/hsd_layout_compat.c) with plain `extern void*`
 * declarations. No-op on Linux -- the Itanium ABI leaves globals in the
 * global namespace unmangled either way -- but MSVC mangles globals
 * (?MEM1Start@@3PEAXEA), so the Windows link would otherwise fail with
 * "undefined symbol: MEM1Start / MEM1End". */
extern "C" {
extern void* MEM1Start;
extern void* MEM1End;
}

void AuroraOSInitMemory();
void AuroraFillBootInfo();
void AuroraInitClock();
void AuroraInitArena();
