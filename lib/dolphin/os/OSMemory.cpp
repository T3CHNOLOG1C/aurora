#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#endif

#include "fmt/base.h"

#include <cassert>

#include "internal.hpp"
#include <dolphin/os.h>
#include "dolphin/types.h"

#if !NDEBUG && (INTPTR_MAX > INT32_MAX)
#define GUARD_MEMORY 1
#endif

uintptr_t OSBaseAddress = 0;

void* MEM1Start;
void* MEM1End;

static void GuardGCMemory();
static void* AllocMEM1(u32 size);

void AuroraOSInitMemory() {
  GuardGCMemory();

  if (aurora::g_config.mem1Size > 0) {
    MEM1Start = AllocMEM1(aurora::g_config.mem1Size);
    MEM1End = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(MEM1Start) + aurora::g_config.mem1Size);
    OSBaseAddress = reinterpret_cast<uintptr_t>(MEM1Start);
  }
}

#if GUARD_MEMORY
static uintptr_t GetAllocationGranularity() {
#if _WIN32
  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);

  return sysInfo.dwAllocationGranularity;
#else
  // TODO: posix impl
  return 0;
#endif
}

static void TryGuardRegion(const uintptr_t start, const uintptr_t end, char const* const name) {
#if _WIN32
  assert(start != 0);
  const auto addr = VirtualAlloc(
      reinterpret_cast<LPVOID>(start),
      end - start,
      MEM_RESERVE,
      PAGE_NOACCESS);

  if (addr == nullptr) {
    Log.debug("Unable to guard memory region: {}", name);
  } else {
    assert(addr == reinterpret_cast<LPVOID>(start));
    Log.debug("Successfully guarded memory range: {:08X}-{:08X} ({})", start, end, name);
  }
#else
  // TODO: posix impl
#endif
}

static void GuardGCMemory() {
  // Reserve the normal GC/Wii memory map so accesses are 100% guaranteed to fail.
  // https://www.gc-forever.com/yagcd/chap5.html#sec5.11
  // https://wiibrew.org/wiki/Memory_map

  // We can't quite map at address 0 (for good reasons) but we *can* map at the next granularity over!
  TryGuardRegion(0x00000000 + GetAllocationGranularity(), 0x017fffff, "MEM1 Physical");
  TryGuardRegion(0x80000000, 0x817fffff, "MEM1 Logical (cached)");
  TryGuardRegion(0xC0000000, 0xC17fffff, "MEM1 Logical (uncached)");
  TryGuardRegion(0x10000000, 0x13FFFFFF, "MEM2 Physical");
  TryGuardRegion(0x90000000, 0x93FFFFFF, "MEM2 Logical (cached)");
  TryGuardRegion(0xD0000000, 0xD3FFFFFF, "MEM2 Logical (uncached)");
  TryGuardRegion(0x08000000, 0x08300000, "EFB Physical");
  TryGuardRegion(0xC8000000, 0xC8300000, "EFB Logical");
  TryGuardRegion(0x0D000000, 0x0D008000, "Hollywood HW registers Physical");
  TryGuardRegion(0xCD000000, 0xCD008000, "Hollywood HW registers Logical");
  TryGuardRegion(0x0C000000, 0x0C008020, "Broadway/GC HW registers Physical");
  TryGuardRegion(0xCC000000, 0xCC008020, "Broadway/GC HW registers Logical");
  TryGuardRegion(0xe0000000, 0xe0003fff, "GC L2 cache");
  TryGuardRegion(0xfff00000, 0xffffffff, "GC IPL");
}
#else
static void GuardGCMemory() { }
#endif

#if _WIN64 && !NDEBUG
static void* AllocMEM1(u32 size) {
  // Allocate an entire 32-bit's worth of memory and allocate the real MEM1 in that.
  // This way, if a 64-bit pointer gets truncated to 32-bit, it will still fall in our guard pages.

  void* bulkChunk = VirtualAlloc(
    nullptr,
    8ll * 1024 * 1024 * 1024,
    MEM_RESERVE,
    PAGE_NOACCESS);

  if (bulkChunk == nullptr) {
    DWORD err = GetLastError();
    fmt::memory_buffer msg;
    fmt::format_system_error(
      msg,
      static_cast<int>(err),
      "Failed to allocate bulk chunk for MEM1");
    Log.fatal("{}", fmt::to_string(msg));
  }

  uintptr_t memSpace = (reinterpret_cast<uintptr_t>(bulkChunk) | 0xFFFFFFFF) + 1;
  void* mem1Address = reinterpret_cast<void*>(memSpace + 0x80000000);

  Log.debug("Reserved memory map at {:016X}-{:016X}", memSpace, memSpace + 0xFFFFFFFF);
  Log.debug(
    "MEM1 at {:016X}-{:016X}",
    reinterpret_cast<uintptr_t>(mem1Address),
    reinterpret_cast<uintptr_t>(mem1Address) + size);

  void* result = VirtualAlloc(mem1Address, size, MEM_COMMIT, PAGE_READWRITE);
  if (result == nullptr) {
    DWORD err = GetLastError();
    fmt::memory_buffer msg;
    fmt::format_system_error(
      msg,
      static_cast<int>(err),
      "Failed to commit memory for MEM1");
    Log.fatal("{}", fmt::to_string(msg));
  }

  assert(result == mem1Address);
  return result;
}
#elif defined(__linux__)
// melee-pc local patch: decomp code (e.g. sysdolphin/baselib/initialize.c's
// HSD_OSInit, `u32 old_arena_lo = (u32) OSGetArenaLo()`) truncates MEM1
// arena pointers to u32 before using them, matching the real SDK's 32-bit
// `void*` -- lossless on the original 32-bit target. A plain calloc() has
// no address guarantee on Linux; large allocations go through mmap and
// commonly land far above 4GB, silently truncating to an unrelated,
// unmapped address (this is what broke `HSD_OSInit`'s `OSCreateHeap` call,
// producing a garbage `HSD_Synth_804D6018` heap handle and an unmapped
// pointer out of the first `OSAllocFromHeap` -- confirmed by checking the
// returned pointer against /proc/<pid>/maps). Request a fixed, low address
// explicitly instead, mirroring what this file already does for Windows
// debug builds above.
//
// That address MUST be exactly 0x80000000, not merely "some low address" --
// this was originally 0x20000000 (still lossless for u32 truncation) and
// that was a real, separate bug: decomp code itself checks addresses
// against the real hardware's cached-MEM1-logical convention directly, not
// just through this file's truncation logic -- e.g. melee/lb/lbfile.c's
// lbFile_800164A4 picks its DMA type via `dest >= 0x80000000`, matching
// real hardware where any legitimate MEM1 destination pointer already
// satisfies that. With MEM1 at 0x20000000, every such check saw the
// buffer as "not MEM1" and routed archive/asset reads through the
// ARAM-relay DMA path instead, with the real destination reinterpreted as
// a bogus multi-hundred-MB ARAM offset -- the actual buffer was left
// all-zeros, silently, until HSD_ArchiveParse's own built-in
// byte-order/size sanity check caught it downstream. 0x80000000 satisfies
// both this file's truncation requirement *and* every one of decomp's own
// "is this a cached MEM1 address" checks, because it's the address real
// hardware actually uses -- there's no other value that's simultaneously
// correct for both.
static void* AllocMEM1(u32 size) {
  void* fixedAddr = reinterpret_cast<void*>(0x80000000);
#if defined(MELEE_PC_DOLSAN_BUILD)
  // DolSAN (extern/dolsan) relocates ASan's shadow layout so it no longer
  // covers this address (see extern/dolsan/cmake/DolSANGekkoProfile.cmake
  // for the reserved range), so unlike plain system ASan below,
  // MAP_FIXED_NOREPLACE is safe here and preferred -- it fails loudly on
  // an unexpected collision instead of silently unmapping whatever's
  // there. DolSAN-linked build only (MELEE_PC_ASAN_BUILD in
  // CMakeLists.txt, which now builds against DolSAN's runtime).
  int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
#elif defined(__SANITIZE_ADDRESS__) || defined(MELEE_PC_ASAN_BUILD)
  // Plain system ASan (not DolSAN): it reserves this address as part of
  // its shadow gap even with ASAN_OPTIONS=protect_shadow_gap=0 (that
  // option only stops ASan from mprotecting it, not from holding a
  // placeholder mapping there), so MAP_FIXED_NOREPLACE always fails.
  // MAP_FIXED unmaps whatever's there first instead of refusing -- safe
  // here specifically because protect_shadow_gap=0 means ASan never
  // relies on that gap being reserved for its own correctness. This does
  // NOT fix the real collision (system ASan's LowShadow still overlaps
  // this address once real writes land here -- see pc_port.md entries
  // (219)/(272) and extern/dolsan/PLANNING.md); kept only for whoever
  // builds aurora_os with plain -fsanitize=address directly, outside
  // melee-pc's own CMake plumbing.
  int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
#else
  int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
#endif
  void* result = mmap(fixedAddr, size, PROT_READ | PROT_WRITE, mmapFlags, -1, 0);
  if (result == MAP_FAILED) {
    Log.fatal("MEM1 mmap errno={} ({}) size={:#x}", errno, strerror(errno), size);
    // No fallback address: anything else would violate decomp's own
    // ">= 0x80000000" MEM1 checks (see above), which is worse than failing
    // loudly here.
    Log.fatal("Failed to allocate MEM1 at 0x80000000 (mmap)");
    return nullptr;
  }
  return result;
}
#else
static void* AllocMEM1(u32 size) {
  return calloc(1, size);
}
#endif

u32 OSGetPhysicalMemSize() {
  const auto info = static_cast<OSBootInfo*>(OSPhysicalToCached(0));
  return info->memorySize;
}
