#ifndef DOLPHIN_TYPES_H
#define DOLPHIN_TYPES_H

/* The real SDK's dolphin/types.h (extern/dolphin/include/dolphin/types.h
 * in the decomp) transitively pulls in <string.h>/<stdio.h>/<stdarg.h>/
 * <ctype.h> from this exact location, and melee code relies on that --
 * dolphin/types.h is included nearly everywhere, so most melee TUs get
 * memcpy/memset/strncmp/va_list etc "for free" without including those
 * headers directly. Aurora's types.h didn't do this. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Likewise, the decomp's <stdio.h> resolves to src/MSL/stdio.h (the
 * Metrowerks C library's own stdio, reached via -isystemsrc/MSL), not the
 * real one -- and that header, not this location, is where
 * sysdolphin/baselib/debug.h's __file_handle/__idle_proc device-driver
 * typedefs actually come from in the original build. debug.h itself
 * doesn't include <MSL/stdio.h>; it just relies on some earlier header in
 * the chain (usually this one, dolphin/types.h, included nearly
 * everywhere) having already pulled it in ambiently. On the real system
 * <stdio.h> those two types don't exist at all (they're MSL/Metrowerks C
 * library internals, part of a custom low-level I/O device-driver
 * interface -- see debug.c's report_func/HSD_LogInit, which hooks
 * MSL's stdout->write_proc; that mechanism has no host libc equivalent
 * and isn't ported, see pc_port.md). Declaring just the two typedefs melee
 * source actually references is enough for debug.h's report_func()
 * prototype (used everywhere via the HSD_ASSERT* macros) to parse; it
 * does NOT make debug.c's MSL-FILE-internals-dependent function bodies
 * portable, those are excluded from the native build separately. */
typedef unsigned long __file_handle;
typedef void (*__idle_proc)(void);
/* Also from src/MSL/stddef.h, same ambient-availability story. */
typedef unsigned int usize_t;

/* From src/MSL/stdarg.h, used directly (not through the standard
 * va_arg macro) by exactly one file: src/melee/ef/efalt.c's
 * EFALT_VA_ARG macro. MSL's va_list on the PPC/MWCC ABI is just a raw
 * pointer, manually advanced; the real host va_list (e.g. x86_64 SysV)
 * is an opaque multi-field struct that can't be safely walked the same
 * way. This stub compiled but trapped loudly if reached, rather than
 * risk quietly reading garbage into particle-effect parameters.
 *
 * NO LONGER REACHED (2026-08-15). efalt.c's EFALT_VA_ARG is redirected
 * to `__builtin_va_arg` by a melee_patch_source in CMakeLists.txt: that
 * TU's `va_list` already resolves to the host's own __builtin_va_list
 * (MSL/stdarg.h is not in its include tree, nor in efsync.c's, which is
 * what calls it), so nothing had to be bridged -- only the macro
 * re-pointed at the real host mechanism instead of at this stub. The
 * stub stays: MSL/stdarg.h still declares __va_arg, and anything that
 * starts using it should keep failing loudly rather than silently. See
 * pc_port.md entry (96) fix 8, which supersedes item 9's "not bridged
 * yet". */
#define _var_arg_typeof(e) 0
/* MSVC claims `__va_arg` as a reserved compiler intrinsic name (error
 * C2169: cannot be defined) -- it isn't user-definable there at all, so
 * this trap-loudly stub is GCC/Clang-only. No decomp TU on this project
 * builds under MSVC (see pc_port.md), so nothing needs it there. */
#if !defined(_MSC_VER) || defined(__clang__)
static inline void* __va_arg(void* list, unsigned char type) {
    (void) list;
    (void) type;
    __builtin_trap();
}
#endif

#if _WIN64 || __LP64__
#define BIT_64 1
#else
#define BIT_64 0
#endif

#ifdef TARGET_PC
#include <stdint.h>
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
typedef signed char s8;
typedef signed short int s16;
typedef signed long s32;
typedef signed long long int s64;
typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned long u32;
typedef unsigned long long int u64;
#endif

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;

typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;

typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef char *Ptr;

#if defined(TARGET_PC)
#include <stdbool.h>
typedef int BOOL;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#endif

#ifdef TARGET_PC
#include <stddef.h>
#else
#ifndef NULL
#define NULL 0
#endif
#endif
#ifndef __cplusplus
#ifndef nullptr
#define nullptr NULL
#endif
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#if defined(__MWERKS__)
#define AT_ADDRESS(addr) : (addr)
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#elif defined(__GNUC__)
#define AT_ADDRESS(addr)
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#elif defined(_MSC_VER)
#define AT_ADDRESS(addr)
#define ATTRIBUTE_ALIGN(num) __declspec(align(num))
#else
#error unknown compiler
#endif

#ifndef DECL_WEAK
#if defined(__MWERKS__)
#define DECL_WEAK __declspec(weak)
#elif defined(__GNUC__)
#define DECL_WEAK __attribute__((weak))
#elif defined(_MSC_VER)
#define DECL_WEAK
#else
#error unknown compiler
#endif
#endif

#if TARGET_PC && __cplusplus
#define NORETURN [[noreturn]]
#else
#define NORETURN
#endif

#ifdef __MWERKS__
#define __REGISTER register
#else
#define __REGISTER
#endif

#endif
