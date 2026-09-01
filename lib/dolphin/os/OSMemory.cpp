#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
#endif

#include "fmt/base.h"

#include <cassert>

#include "internal.hpp"
#include <dolphin/os.h>
#include "dolphin/types.h"

#if !NDEBUG && (INTPTR_MAX > INT32_MAX)
#define GUARD_MEMORY 1
#endif

/* MEM1Start/MEM1End are declared extern "C" in internal.hpp -- see the comment
 * there for why (MSVC mangles globals; the C compat layer references them). */
extern "C" uintptr_t OSBaseAddress = 0;

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
  // NOT guarded: 0x80000000-0x817fffff, "MEM1 Logical (cached)". That is where
  // real MEM1 is now mapped on every platform (see kMEM1Address below), so
  // reserving it here would make the subsequent AllocMEM1 fail. Guarding it and
  // placing MEM1 there are mutually exclusive by construction, and placing it
  // there is the requirement -- see the kMEM1Address comment for why.
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

// OpenMelee local patch: MEM1 must be mapped at exactly 0x80000000, on every
// platform. This is not a preference or a debugging aid -- two independent
// requirements pin it to this one value:
//
//  1. Truncation. Decomp code (e.g. sysdolphin/baselib/initialize.c's
//     HSD_OSInit, `u32 old_arena_lo = (u32) OSGetArenaLo()`) truncates MEM1
//     arena pointers to u32 before using them, matching the real SDK's 32-bit
//     `void*` -- lossless on the original 32-bit target. A plain calloc() has
//     no address guarantee; large allocations go through mmap/HeapAlloc and
//     commonly land far above 4GB, silently truncating to an unrelated,
//     unmapped address. That is what broke `HSD_OSInit`'s `OSCreateHeap` call,
//     producing a garbage `HSD_Synth_804D6018` heap handle and an unmapped
//     pointer out of the first `OSAllocFromHeap` -- confirmed by checking the
//     returned pointer against /proc/<pid>/maps. So MEM1 must be below 4GB.
//
//  2. Decomp's own MEM1 range checks. Being merely "some low address" is not
//     enough -- this was originally 0x20000000 (still lossless for u32
//     truncation) and that was a real, separate bug. Decomp checks addresses
//     against the real hardware's cached-MEM1-logical convention directly, not
//     just through this file's truncation logic -- e.g. melee/lb/lbfile.c's
//     lbFile_800164A4 picks its DMA type via `dest >= 0x80000000`, matching
//     real hardware where any legitimate MEM1 destination pointer already
//     satisfies that. With MEM1 at 0x20000000, every such check saw the buffer
//     as "not MEM1" and routed archive/asset reads through the ARAM-relay DMA
//     path instead, with the real destination reinterpreted as a bogus
//     multi-hundred-MB ARAM offset -- the actual buffer was left all-zeros,
//     silently, until HSD_ArchiveParse's own byte-order/size sanity check
//     caught it downstream.
//
// 0x80000000 is the only value satisfying both, because it is the address real
// hardware actually uses. There is deliberately no fallback address anywhere
// below: failing to boot loudly is far better than booting into the
// silently-corrupt states described above.
static constexpr uintptr_t kMEM1Address = 0x80000000;

// 0x80000000 is kernel-reserved in a 32-bit address space, so there is no
// arrangement that satisfies the above on a 32-bit host.
static_assert(INTPTR_MAX > INT32_MAX, "MEM1 at 0x80000000 requires a 64-bit build");

#if _WIN32
static void* AllocMEM1(u32 size) {
  // In a 64-bit process 0x80000000 is ordinary, free user address space, and
  // it is 64KB-granularity aligned, so a single reserve+commit is all that is
  // needed. (This replaced an older debug-only scheme that reserved 8GB and
  // placed MEM1 at `bulkBase + 0x80000000` so truncated pointers would land in
  // guard pages. That is a good sanitizer trick for other titles, but it is
  // incompatible with melee: truncating such a pointer to u32 discards
  // `bulkBase`, which is exactly requirement 1 above.)
  void* const fixedAddr = reinterpret_cast<void*>(kMEM1Address);
  void* result = VirtualAlloc(fixedAddr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (result == nullptr) {
    const DWORD err = GetLastError();
    fmt::memory_buffer msg;
    fmt::format_system_error(
      msg,
      static_cast<int>(err),
      "Failed to allocate MEM1 at 0x80000000");
    Log.fatal("{}", fmt::to_string(msg));
    return nullptr;
  }
  assert(result == fixedAddr);
  Log.debug("MEM1 at {:016X}-{:016X}", kMEM1Address, kMEM1Address + size);
  return result;
}
#else

#if !defined(MAP_FIXED_NOREPLACE)
// mincore() reports 0 only when the entire range is mapped, and fails with
// ENOMEM when any part of it is not. That is a conservative occupancy probe
// rather than an exact one, but it is enough to turn "MAP_FIXED silently
// unmapped somebody else's allocation" into a loud failure on platforms
// without Linux's MAP_FIXED_NOREPLACE (macOS, iOS, the BSDs).
static bool RegionIsFree(void* addr, size_t size) {
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return false;
  }
  std::vector<char> vec((size + static_cast<size_t>(pageSize) - 1) / static_cast<size_t>(pageSize));
  errno = 0;
  if (mincore(addr, size, vec.data()) == 0) {
    return false; // fully mapped -- definitely occupied
  }
  return errno == ENOMEM; // nothing mapped in at least part of the range
}
#endif

static void* AllocMEM1(u32 size) {
  void* const fixedAddr = reinterpret_cast<void*>(kMEM1Address);
  int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;

#if defined(OPENMELEE_DOLSAN_BUILD)
  // DolSAN (extern/dolsan) relocates ASan's shadow layout so it no longer
  // covers this address (see extern/dolsan/cmake/DolSANGekkoProfile.cmake for
  // the reserved range), so unlike plain system ASan below,
  // MAP_FIXED_NOREPLACE is safe here and preferred -- it fails loudly on an
  // unexpected collision instead of silently unmapping whatever's there.
  // DolSAN-linked build only (OPENMELEE_ASAN_BUILD in CMakeLists.txt, which now
  // builds against DolSAN's runtime).
  mmapFlags |= MAP_FIXED_NOREPLACE;
#elif defined(__SANITIZE_ADDRESS__) || defined(OPENMELEE_ASAN_BUILD)
  // Plain system ASan (not DolSAN): it reserves this address as part of its
  // shadow gap even with ASAN_OPTIONS=protect_shadow_gap=0 (that option only
  // stops ASan from mprotecting it, not from holding a placeholder mapping
  // there), so MAP_FIXED_NOREPLACE always fails, and the occupancy probe below
  // would always refuse. MAP_FIXED unmaps whatever's there first instead --
  // safe here specifically because protect_shadow_gap=0 means ASan never
  // relies on that gap for its own correctness. This does NOT fix the real
  // collision (system ASan's LowShadow still overlaps this address once real
  // writes land here -- see pc_port.md entries (219)/(272) and
  // extern/dolsan/PLANNING.md); kept only for whoever builds aurora_os with
  // plain -fsanitize=address directly, outside OpenMelee's own CMake plumbing.
  mmapFlags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
  mmapFlags |= MAP_FIXED_NOREPLACE;
#else
  // Non-Linux POSIX. Probe first, since MAP_FIXED here would clobber rather
  // than refuse.
  if (!RegionIsFree(fixedAddr, size)) {
    Log.fatal("MEM1 range 0x80000000+{:#x} is already mapped; refusing to clobber it", size);
    return nullptr;
  }
  mmapFlags |= MAP_FIXED;
#endif

  void* result = mmap(fixedAddr, size, PROT_READ | PROT_WRITE, mmapFlags, -1, 0);
  if (result == MAP_FAILED) {
    Log.fatal("MEM1 mmap errno={} ({}) size={:#x}", errno, strerror(errno), size);
#if defined(__APPLE__)
    // Mach-O's default __PAGEZERO is 4GB on arm64 (and 4KB on x86_64), which
    // makes every address below 0x100000000 permanently unmappable. Link the
    // final executable with `-Wl,-pagezero_size,0x1000` to shrink it.
    Log.fatal("On macOS/iOS this usually means __PAGEZERO covers the low 4GB -- "
              "link with -Wl,-pagezero_size,0x1000");
#endif
    Log.fatal("Failed to allocate MEM1 at 0x80000000 (mmap)");
    return nullptr;
  }
  Log.debug("MEM1 at {:016X}-{:016X}", kMEM1Address, kMEM1Address + size);
  return result;
}
#endif

u32 OSGetPhysicalMemSize() {
  const auto info = static_cast<OSBootInfo*>(OSPhysicalToCached(0));
  return info->memorySize;
}
