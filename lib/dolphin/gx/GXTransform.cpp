#include "gx.hpp"
#include "__gx.h"
#include "dolphin/mtx/GeoTypes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#if !defined(_WIN32)
#include <execinfo.h>
#endif

static inline void CacheProjectionVector(const f32* ptr, GXProjectionType type) {
  __gx->projType = type;
  for (int i = 0; i < 6; ++i) {
    __gx->projMtx[i] = ptr[i + 1];
  }
}

/* TEMPORARY diagnostic (2026-08-11): one-shot trace of the transform state
 * feeding the vertex pipeline, for the "everything renders as huge distorted
 * polygons" investigation. Gated behind MELEE_PC_TRACE_MTX=<count>. */
static int melee_trace_mtx_budget() {
  static int budget = -1;
  if (budget < 0) {
    const char* env = std::getenv("MELEE_PC_TRACE_MTX");
    budget = env != nullptr ? std::atoi(env) : 0;
  }
  return budget;
}
static int melee_trace_proj_count = 0;
static int melee_trace_pos_count = 0;

/* TEMPORARY diagnostic (2026-08-14): "the stage and fighters never appear,
 * only the (screen-space) HUD does" investigation. The per-call MTX trace
 * above is useless for that question -- it burns its whole budget inside the
 * first few frames of the boot logos, thousands of frames before the match.
 * This one instead prints a line only when the combined
 * viewport+projection state actually *changes*, so a whole run collapses to
 * the handful of distinct camera setups the frame is built from (3D scene
 * camera, HUD ortho camera, ...) no matter how long it runs.
 * Gated behind MELEE_PC_TRACE_VP=1. */
static int melee_trace_vp_level() {
  static int level = -1;
  if (level < 0) {
    const char* env = std::getenv("MELEE_PC_TRACE_VP");
    level = env != nullptr ? std::max(1, std::atoi(env)) : 0;
  }
  return level;
}

static bool melee_trace_vp_enabled() { return melee_trace_vp_level() > 0; }

/* At level >= 2, a projection containing a non-finite value (an infinite
 * cot(fov/2), i.e. a camera whose field of view came out as 0) also prints a
 * native backtrace, which is the only practical way to find *which* of the
 * several cameras composing a frame is the broken one -- gdb cannot usefully
 * put a conditional breakpoint here on this optimized build. Budgeted so a
 * per-frame bug does not produce a per-frame backtrace. */
static void melee_trace_vp_backtrace() {
#if !defined(_WIN32)
  static int budget = 3;
  if (melee_trace_vp_level() < 2 || budget <= 0) {
    return;
  }
  --budget;
  void* frames[32];
  const int n = backtrace(frames, 32);
  std::fprintf(stderr, "PROGDBG VP NONFINITE backtrace (%d frames):\n", n);
  backtrace_symbols_fd(frames, n, fileno(stderr));
#endif
}

static void melee_trace_vp(GXProjectionType type, const f32* projVec) {
  if (!melee_trace_vp_enabled()) {
    return;
  }
  static f32 lastProj[7];
  static f32 lastVp[6];
  static bool primed = false;
  const f32 vp[6] = {__gx->vpLeft, __gx->vpTop, __gx->vpWd, __gx->vpHt, __gx->vpNearz, __gx->vpFarz};
  if (primed && std::memcmp(lastProj, projVec, sizeof(lastProj)) == 0 &&
      std::memcmp(lastVp, vp, sizeof(lastVp)) == 0) {
    return;
  }
  std::memcpy(lastProj, projVec, sizeof(lastProj));
  std::memcpy(lastVp, vp, sizeof(lastVp));
  primed = true;
  std::fprintf(stderr,
               "PROGDBG VP type=%d vp=[l=%g t=%g w=%g h=%g nz=%g fz=%g] "
               "proj=[%g %g %g %g %g %g]\n",
               static_cast<int>(type), vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], projVec[1], projVec[2],
               projVec[3], projVec[4], projVec[5], projVec[6]);
  for (int i = 1; i <= 6; ++i) {
    if (!std::isfinite(projVec[i])) {
      melee_trace_vp_backtrace();
      break;
    }
  }
}

extern "C" {

void GXSetProjection(const void* mtx_, GXProjectionType type) {
  const auto& mtx = *reinterpret_cast<const aurora::Mat4x4<float>*>(mtx_);
  if (melee_trace_proj_count < melee_trace_mtx_budget()) {
    ++melee_trace_proj_count;
    std::fprintf(stderr,
                 "PROGDBG PROJ type=%d [%g %g %g %g][%g %g %g %g][%g %g %g %g][%g %g %g %g]\n",
                 static_cast<int>(type), mtx[0][0], mtx[0][1], mtx[0][2], mtx[0][3], mtx[1][0], mtx[1][1],
                 mtx[1][2], mtx[1][3], mtx[2][0], mtx[2][1], mtx[2][2], mtx[2][3], mtx[3][0], mtx[3][1],
                 mtx[3][2], mtx[3][3]);
  }
  const f32 projVec[] = {
      static_cast<f32>(type == GX_ORTHOGRAPHIC),
      mtx[0][0],
      type == GX_ORTHOGRAPHIC ? mtx[0][3] : mtx[0][2],
      mtx[1][1],
      type == GX_ORTHOGRAPHIC ? mtx[1][3] : mtx[1][2],
      mtx[2][2],
      mtx[2][3],
  };
  CacheProjectionVector(projVec, type);
  melee_trace_vp(type, projVec);

  // XF bulk write: 6 params + projection type at 0x1020-0x1026
  GX_WRITE_U8(0x10);
  GX_WRITE_U32(0x00061020);
  GX_WRITE_XF_REG_F(32, __gx->projMtx[0]);
  GX_WRITE_XF_REG_F(33, __gx->projMtx[1]);
  GX_WRITE_XF_REG_F(34, __gx->projMtx[2]);
  GX_WRITE_XF_REG_F(35, __gx->projMtx[3]);
  GX_WRITE_XF_REG_F(36, __gx->projMtx[4]);
  GX_WRITE_XF_REG_F(37, __gx->projMtx[5]);
  GX_WRITE_XF_REG_2(38, __gx->projType);
  __gx->bpSent = 0;
}

void GXSetProjectionv(const f32* ptr) {
  CHECK(ptr != nullptr, "null projection vector");

  const GXProjectionType type = ptr[0] == 0.0f ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC;
  CacheProjectionVector(ptr, type);
  melee_trace_vp(type, ptr);

  // XF bulk write: 6 params + projection type at 0x1020-0x1026
  GX_WRITE_U8(0x10);
  GX_WRITE_U32(0x00061020);
  for (int i = 0; i < 6; ++i) {
    GX_WRITE_F32(__gx->projMtx[i]);
  }
  GX_WRITE_U32(__gx->projType);
  __gx->bpSent = 0;
}

void GXLoadPosMtxImm(const void* mtx_, u32 id) {
  CHECK(id >= GX_PNMTX0 && id <= GX_PNMTX9, "invalid pn mtx {}", static_cast<int>(id));
  const auto* mtx = reinterpret_cast<const f32*>(mtx_);
  if (melee_trace_pos_count < melee_trace_mtx_budget()) {
    ++melee_trace_pos_count;
    std::fprintf(stderr, "PROGDBG POSMTX id=%u [%g %g %g %g][%g %g %g %g][%g %g %g %g]\n", id, mtx[0], mtx[1],
                 mtx[2], mtx[3], mtx[4], mtx[5], mtx[6], mtx[7], mtx[8], mtx[9], mtx[10], mtx[11]);
  }

  GX_WRITE_U8(0x10);
  GX_WRITE_U32((id * 4) | 0xB0000);
  for (int i = 0; i < 12; i++) {
    GX_WRITE_F32(mtx[i]);
  }
}

void GXLoadPosMtxIndx(u16 mtxIndx, u32 id) {
  CHECK(id >= GX_PNMTX0 && id <= GX_PNMTX9, "invalid pn mtx {}", static_cast<int>(id));

  GX_WRITE_U8(GX_LOAD_INDX_A);
  GX_WRITE_U16(mtxIndx);
  GX_WRITE_U16((11u << 12) | id * 4);
}

void GXLoadNrmMtxImm(const void* mtx_, u32 id) {
  CHECK(id >= GX_PNMTX0 && id <= GX_PNMTX9, "invalid pn mtx {}", static_cast<int>(id));
  const auto* mtx = reinterpret_cast<const f32*>(mtx_);

  GX_WRITE_U8(0x10);
  GX_WRITE_U32((id * 3 + 0x400) | 0x80000);
  // Write 3x3 from 3x4 matrix (skip translation column)
  GX_WRITE_F32(mtx[0]);
  GX_WRITE_F32(mtx[1]);
  GX_WRITE_F32(mtx[2]);
  GX_WRITE_F32(mtx[4]);
  GX_WRITE_F32(mtx[5]);
  GX_WRITE_F32(mtx[6]);
  GX_WRITE_F32(mtx[8]);
  GX_WRITE_F32(mtx[9]);
  GX_WRITE_F32(mtx[10]);
}

void GXSetCurrentMtx(u32 id) {
  CHECK(id >= GX_PNMTX0 && id <= GX_PNMTX9, "invalid pn mtx {}", id);
  SET_REG_FIELD(0, __gx->matIdxA, 6, 0, id);
  __GXSetMatrixIndex(GX_VA_PNMTXIDX);
}

void GXLoadTexMtxImm(const void* mtx_, u32 id, GXTexMtxType type) {
  CHECK((id >= GX_TEXMTX0 && id <= GX_IDENTITY) || (id >= GX_PTTEXMTX0 && id <= GX_PTIDENTITY), "invalid tex mtx {}",
        id);

  u32 addr;
  if (id >= GX_PTTEXMTX0) {
    addr = (id - GX_PTTEXMTX0) * 4 + 0x500;
    CHECK(type == GX_MTX3x4, "invalid pt mtx type {}", underlying(type));
  } else {
    addr = id * 4;
  }

  u32 count = (type == GX_MTX2x4) ? 8 : 12;
  u32 reg = addr | ((count - 1) << 16);

  GX_WRITE_U8(0x10);
  GX_WRITE_U32(reg);

  const auto* mtx = reinterpret_cast<const f32*>(mtx_);
  for (u32 i = 0; i < count; i++) {
    GX_WRITE_F32(mtx[i]);
  }
}

void GXSetViewport(float left, float top, float width, float height, float nearZ, float farZ) {
  GXSetViewportJitter(left, top, width, height, nearZ, farZ, 1);
}

void GXSetViewportJitter(float left, float top, float width, float height, float nearZ, float farZ, u32 field) {
  float sx;
  float sy;
  float sz;
  float ox;
  float oy;
  float oz;
  float zmin;
  float zmax;

  if (field == 0) {
    top -= 0.5f;
  }

  sx = width / 2.0f;
  sy = -height / 2.0f;
  ox = 340.0f + (left + width / 2.0f);
  oy = 340.0f + (top + height / 2.0f);
  zmin = 1.6777215e7f * nearZ;
  zmax = 1.6777215e7f * farZ;
  sz = zmax - zmin;
  oz = zmax;

  __gx->vpLeft = left;
  __gx->vpTop = top;
  __gx->vpWd = width;
  __gx->vpHt = height;
  __gx->vpNearz = nearZ;
  __gx->vpFarz = farZ;

  GX_WRITE_U8(0x10);
  GX_WRITE_U32(0x0005101A);
  GX_WRITE_XF_REG_F(26, sx);
  GX_WRITE_XF_REG_F(27, sy);
  GX_WRITE_XF_REG_F(28, sz);
  GX_WRITE_XF_REG_F(29, ox);
  GX_WRITE_XF_REG_F(30, oy);
  GX_WRITE_XF_REG_F(31, oz);
  __gx->bpSent = 0;
}

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx,
               f32* sy, f32* sz) {
  Vec peye;
  f32 xc;
  f32 yc;
  f32 zc;
  f32 wc;

  peye.x = mtx[0][3] + ((mtx[0][2] * z) + ((mtx[0][0] * x) + (mtx[0][1] * y)));
  peye.y = mtx[1][3] + ((mtx[1][2] * z) + ((mtx[1][0] * x) + (mtx[1][1] * y)));
  peye.z = mtx[2][3] + ((mtx[2][2] * z) + ((mtx[2][0] * x) + (mtx[2][1] * y)));
  if (pm[0] == 0.0f) {
    xc = (peye.x * pm[1]) + (peye.z * pm[2]);
    yc = (peye.y * pm[3]) + (peye.z * pm[4]);
    zc = pm[6] + (peye.z * pm[5]);
    wc = 1.0f / -peye.z;
  } else {
    xc = pm[2] + (peye.x * pm[1]);
    yc = pm[4] + (peye.y * pm[3]);
    zc = pm[6] + (peye.z * pm[5]);
    wc = 1.0f;
  }
  *sx = (vp[2] / 2.0f) + (vp[0] + (wc * (xc * vp[2] / 2.0f)));
  *sy = (vp[3] / 2.0f) + (vp[1] + (wc * (-yc * vp[3] / 2.0f)));
  *sz = vp[5] + (wc * (zc * (vp[5] - vp[4])));
}

// TODO GXLoadNrmMtxImm3x3
// TODO GXLoadNrmMtxIndx3x3
// TODO GXLoadTexMtxIndx
// TODO GXSetZScaleOffset
}
