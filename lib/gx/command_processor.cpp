#include "command_processor.hpp"

#include "../gfx/common.hpp"
#include "../gfx/depth_peek.hpp"
#include "../internal.hpp"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "fifo.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <tracy/Tracy.hpp>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

class Reader {
public:
  explicit Reader(std::span<const u8> data) noexcept : mData{data} {}

  [[nodiscard]] bool empty() const noexcept { return mPos == mData.size(); }
  [[nodiscard]] size_t offset() const noexcept { return mPos; }
  [[nodiscard]] size_t size() const noexcept { return mData.size(); }
  [[nodiscard]] size_t remaining() const noexcept { return mData.size() - mPos; }
  [[nodiscard]] const u8* data() const noexcept { return mData.data(); }

  template <typename T>
    requires(std::is_arithmetic_v<T>)
  T read() noexcept {
    const auto bytes = take(sizeof(T));
    return read_bits<T>(bytes.data());
  }

  std::span<const u8> take(size_t count) noexcept {
    AURORA_ASSERT(count <= remaining(), "FIFO read overrun: need {} bytes at offset {}, have {}", count, mPos,
                  remaining());
    const auto bytes = mData.subspan(mPos, count);
    mPos += count;
    return bytes;
  }

  void skip(size_t count) noexcept {
    AURORA_ASSERT(count <= remaining(), "FIFO read overrun: need {} bytes at offset {}, have {}", count, mPos,
                  remaining());
    mPos += count;
  }

  std::string read_string() noexcept {
    const auto len = read<uint16_t>();
    const auto bytes = take(len);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

private:
  std::span<const u8> mData;
  size_t mPos = 0;
};

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  if (prim == GX_QUADS) {
    buf.reserve_extra((vtxCount / 4) * 6 * sizeof(u16));

    for (u16 v = 0; v < vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct DrawCache {
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

struct ArrayUploadKey {
  const void* data;
  bool le;
  bool wordSwapped;

  bool operator==(const ArrayUploadKey& rhs) const {
    return data == rhs.data && le == rhs.le && wordSwapped == rhs.wordSwapped;
  }
};

struct ArrayUploadKeyHash {
  size_t operator()(const ArrayUploadKey& key) const {
    size_t h = std::hash<const void*>{}(key.data);
    h ^= static_cast<size_t>(key.le) << 1;
    h ^= static_cast<size_t>(key.wordSwapped) << 2;
    return h;
  }
};

std::unordered_map<ArrayUploadKey, gfx::Range, ArrayUploadKeyHash> sArrayUploadCache;

/* melee-pc: env-gated draw probe for the Underground Maze render bug.
 * For indexed-POS draws only (a stale POS array binding on a direct-vertex draw
 * is meaningless), decodes the first few vertices exactly as the shader will and
 * dumps them alongside the active position matrix.  Sane vertices with a garbage
 * matrix means the transform path; garbage vertices means the fetch path.
 * Logged once per unique (array, format, matrix-contents) tuple so a whole match
 * stays readable.  Remove once the bug is closed. */

/* melee-pc: probes below dedupe per array for the whole run, so an object first
 * drawn in an earlier room never re-logs its state later.  SIGUSR1 bumps this
 * generation, which is folded into every dedupe key, so `kill -USR1 <pid>` takes
 * a fresh snapshot of whatever is on screen right now. */
std::atomic<u32> sDiagGen{0};

void gxdiag_rearm_handler(int) noexcept { sDiagGen.fetch_add(1, std::memory_order_relaxed); }

u32 gxdiag_gen() noexcept {
#if !defined(_WIN32)
  /* Windows has no SIGUSR1, so the `kill -USR1 <pid>` re-arm is POSIX-only.
   * The generation counter itself still works everywhere -- it just never
   * advances on Windows, which degrades this probe to "log each unique tuple
   * once per run" rather than breaking it. */
  static const bool installed = [] {
    std::signal(SIGUSR1, gxdiag_rearm_handler);
    return true;
  }();
  (void) installed;
#endif
  return sDiagGen.load(std::memory_order_relaxed);
}


/* melee-pc: per-frame draw-call bisection.  With MELEE_PC_GXDRAWMAX set, the cap
 * is re-read every frame from /tmp/melee-drawmax so it can be changed live while
 * the game runs (`echo 250 > /tmp/melee-drawmax`) instead of needing a restart.
 * A negative or missing value means "draw everything".  The draw sitting at the
 * cap boundary is logged, so stepping the cap until the orange appears names the
 * exact draw that produces it.  Diagnostic only. */
int sDrawMax = -1;
u32 sFrameDrawIndex = 0;

bool gxdiag_drawmax_active() noexcept {
  static const bool on = getenv("MELEE_PC_GXDRAWMAX") != nullptr;
  return on;
}

int sSkipLo = -1;
int sSkipHi = -1;
u32 sRangeLogFrames = 0;

/* Re-read the skip range from /tmp/melee-skiprange each frame so the cluster can
 * be bisected live (`echo 166:170 > /tmp/melee-skiprange`). */
void gxdiag_skiprange_refresh() noexcept {
  FILE* f = fopen("/tmp/melee-skiprange", "r");
  if (f == nullptr) {
    sSkipLo = sSkipHi = -1;
    return;
  }
  int lo = -1, hi = -1;
  const int n = fscanf(f, "%d:%d", &lo, &hi);
  fclose(f);
  const int prevLo = sSkipLo, prevHi = sSkipHi;
  if (n == 2) {
    sSkipLo = lo;
    sSkipHi = hi;
  } else if (n == 1) {
    sSkipLo = sSkipHi = lo;
  } else {
    sSkipLo = sSkipHi = -1;
  }
  if (sSkipLo != prevLo || sSkipHi != prevHi) {
    sRangeLogFrames = 2;
  } else if (sRangeLogFrames > 0) {
    --sRangeLogFrames;
  }
}

void gxdiag_drawmax_refresh() noexcept {
  if (!gxdiag_drawmax_active()) {
    return;
  }
  FILE* f = fopen("/tmp/melee-drawmax", "r");
  if (f == nullptr) {
    sDrawMax = -1;
    return;
  }
  int v = -1;
  if (fscanf(f, "%d", &v) != 1) {
    v = -1;
  }
  fclose(f);
  sDrawMax = v;
}


/* melee-pc: raw vertex bytes of the most recent immediate-mode draw, so the
 * bisected draw can be decoded without re-reading the FIFO. */
char sLastDirectPos[64] = "?";
u8 sLastVtxBytes[256];
u32 sLastVtxLen = 0;
u32 sLastVtxSize = 0;

bool gxdiag_enabled() noexcept {
  static const bool on = getenv("MELEE_PC_GXDIAG") != nullptr;
  return on;
}

float gxdiag_decode(const u8* p, bool littleEndian, GXCompType type, u8 frac) noexcept {
  switch (type) {
  case GX_F32: {
    u32 w;
    memcpy(&w, p, 4);
    if (!littleEndian) {
      w = bswap(w);
    }
    float f;
    memcpy(&f, &w, 4);
    return f;
  }
  case GX_U16:
  case GX_S16: {
    u16 h;
    memcpy(&h, p, 2);
    if (!littleEndian) {
      h = bswap(h);
    }
    const float v = type == GX_S16 ? static_cast<float>(static_cast<s16>(h)) : static_cast<float>(h);
    return v / static_cast<float>(1u << frac);
  }
  case GX_U8:
  case GX_S8: {
    const float v = type == GX_S8 ? static_cast<float>(static_cast<s8>(*p)) : static_cast<float>(*p);
    return v / static_cast<float>(1u << frac);
  }
  default:
    return 0.f;
  }
}

/* True when a decoded triple looks like plausible model-space geometry. */
bool gxdiag_plausible(const float* v, u32 cnt) noexcept {
  for (u32 c = 0; c < cnt; ++c) {
    if (!std::isfinite(v[c]) || std::fabs(v[c]) > 1.0e5f) {
      return false;
    }
  }
  return true;
}

void gxdiag_probe_pos(GXVtxFmt fmt) noexcept {
  const auto desc = g_gxState.vtxDesc[GX_VA_POS];
  if (desc != GX_INDEX8 && desc != GX_INDEX16) {
    return;
  }
  const auto& array = g_gxState.arrays[GX_VA_POS];
  if (array.data == nullptr || array.stride == 0) {
    return;
  }
  const auto& attrFmt = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];

  static std::set<std::tuple<u32, const void*, u8, u8, u8>> seen;
  if (!seen.insert(std::make_tuple(gxdiag_gen(), array.data, array.stride, static_cast<u8>(attrFmt.cnt),
                                   static_cast<u8>(attrFmt.type)))
           .second) {
    return;
  }

  /* Effective byte order the shader sees: wordSwapped arrays are byte-reversed
   * into the upload copy and then read big-endian, which is exactly the
   * little-endian interpretation of the bytes still sitting in guest memory. */
  const bool shaderLE = array.le || array.wordSwapped;

  const u32 cnt = comp_cnt_count(GX_VA_POS, attrFmt.cnt);
  const u32 compSize = comp_type_size(GX_VA_POS, attrFmt.type);
  const auto* base = static_cast<const u8*>(array.data);

  // Score both interpretations over the first 16 vertices.
  u32 okLE = 0, okBE = 0, n = 0;
  for (u32 v = 0; v < 16; ++v) {
    const u8* rec = base + static_cast<size_t>(v) * array.stride;
    float le[3]{}, be[3]{};
    for (u32 c = 0; c < cnt && c < 3; ++c) {
      le[c] = gxdiag_decode(rec + c * compSize, true, attrFmt.type, attrFmt.frac);
      be[c] = gxdiag_decode(rec + c * compSize, false, attrFmt.type, attrFmt.frac);
    }
    okLE += gxdiag_plausible(le, std::min(cnt, 3u));
    okBE += gxdiag_plausible(be, std::min(cnt, 3u));
    ++n;
  }

  std::string hex;
  for (u32 i = 0; i < 24; ++i) {
    hex += fmt::format("{:02x}{}", base[i], (i % 4) == 3 ? " " : "");
  }
  std::string sample;
  for (u32 v = 0; v < 2; ++v) {
    const u8* rec = base + static_cast<size_t>(v) * array.stride;
    sample += " LE(";
    for (u32 c = 0; c < cnt; ++c) {
      sample += fmt::format("{}{:.2f}", c ? "," : "", gxdiag_decode(rec + c * compSize, true, attrFmt.type, attrFmt.frac));
    }
    sample += ") BE(";
    for (u32 c = 0; c < cnt; ++c) {
      sample += fmt::format("{}{:.2f}", c ? "," : "", gxdiag_decode(rec + c * compSize, false, attrFmt.type, attrFmt.frac));
    }
    sample += ")";
  }

  /* Fragment/depth/material state for this array's first draw: a correctly
   * decoded mesh that still paints the screen flat points here, not at geometry. */
  const auto& tex0 = g_gxState.loadedTextures[0];
  const auto& c0 = g_gxState.colorRegs[0];
  Log.error("GXSTATE array={} tevStages={} texGens={} chans={} tex0={} texFmt={} tex0size={}x{} "
            "depthCmp={} depthUpd={} depthFunc={} cull={} blend={} src={} dst={} alphaCmp=({},{},{},{},{}) "
            "colorUpd={} creg0=({:.2f},{:.2f},{:.2f},{:.2f}) projType={} proj0=[{:.3f} {:.3f} {:.3f} {:.3f}] "
            "proj2=[{:.3f} {:.3f} {:.3f} {:.3f}] proj3=[{:.3f} {:.3f} {:.3f} {:.3f}]",
            array.data, g_gxState.numTevStages, g_gxState.numTexGens, g_gxState.numChans,
            static_cast<const void*>(tex0.data), static_cast<u32>(tex0.mFormat), tex0.mWidth, tex0.mHeight,
            g_gxState.depthCompare, g_gxState.depthUpdate, static_cast<u32>(g_gxState.depthFunc),
            static_cast<u32>(g_gxState.cullMode), static_cast<u32>(g_gxState.blendMode),
            static_cast<u32>(g_gxState.blendFacSrc), static_cast<u32>(g_gxState.blendFacDst),
            static_cast<u32>(g_gxState.alphaCompare.comp0), g_gxState.alphaCompare.ref0,
            static_cast<u32>(g_gxState.alphaCompare.op), static_cast<u32>(g_gxState.alphaCompare.comp1),
            g_gxState.alphaCompare.ref1, g_gxState.colorUpdate, c0.m[0], c0.m[1], c0.m[2], c0.m[3],
            static_cast<u32>(g_gxState.projType), g_gxState.proj[0][0], g_gxState.proj[0][1], g_gxState.proj[0][2],
            g_gxState.proj[0][3], g_gxState.proj[2][0], g_gxState.proj[2][1], g_gxState.proj[2][2],
            g_gxState.proj[2][3], g_gxState.proj[3][0], g_gxState.proj[3][1], g_gxState.proj[3][2],
            g_gxState.proj[3][3]);

  /* Project this mesh's vertices to NDC exactly as the pipeline will, and report
   * the screen-space bounding box.  Identifies which draws blanket the screen
   * instead of guessing from object identity. */
  {
    const auto& pm = g_gxState.pnMtx[g_gxState.currentPnMtx].pos;
    float m[12];
    memcpy(m, &pm, sizeof(m));
    const auto& P = g_gxState.proj;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    u32 behind = 0, counted = 0;
    for (u32 v = 0; v < 64; ++v) {
      const u8* rec = static_cast<const u8*>(array.data) + static_cast<size_t>(v) * array.stride;
      float o[3]{};
      for (u32 c = 0; c < cnt && c < 3; ++c) {
        o[c] = gxdiag_decode(rec + c * compSize, shaderLE, attrFmt.type, attrFmt.frac);
      }
      if (!gxdiag_plausible(o, std::min(cnt, 3u))) {
        continue;
      }
      const float ex = m[0] * o[0] + m[1] * o[1] + m[2] * o[2] + m[3];
      const float ey = m[4] * o[0] + m[5] * o[1] + m[6] * o[2] + m[7];
      const float ez = m[8] * o[0] + m[9] * o[1] + m[10] * o[2] + m[11];
      const float cw = P[3][0] * ex + P[3][1] * ey + P[3][2] * ez + P[3][3];
      if (cw <= 0.0001f) {
        ++behind;
        continue;
      }
      const float cx = P[0][0] * ex + P[0][1] * ey + P[0][2] * ez + P[0][3];
      const float cy = P[1][0] * ex + P[1][1] * ey + P[1][2] * ez + P[1][3];
      const float nx = cx / cw, ny = cy / cw;
      if (!std::isfinite(nx) || !std::isfinite(ny)) {
        continue;
      }
      lo[0] = std::min(lo[0], nx);
      hi[0] = std::max(hi[0], nx);
      lo[1] = std::min(lo[1], ny);
      hi[1] = std::max(hi[1], ny);
      ++counted;
    }
    if (counted > 0) {
      const float w = hi[0] - lo[0], h = hi[1] - lo[1];
      Log.error("GXCOV array={} n={} behind={} ndcX[{:.2f},{:.2f}] ndcY[{:.2f},{:.2f}] w={:.2f} h={:.2f} {}",
                array.data, counted, behind, lo[0], hi[0], lo[1], hi[1], w, h,
                (w > 2.0f && h > 2.0f) ? "SCREENFILL" : "");
    } else {
      Log.error("GXCOV array={} n=0 behind={} (nothing in front of camera)", array.data, behind);
    }
  }

  /* Shading inputs.  Positions have been verified correct end to end, so a surface
   * rendering as flat orange must take that colour from vertex colours, the
   * lighting channel, or the light objects -- none of which any earlier probe
   * examined. */
  {
    const auto clrDesc = g_gxState.vtxDesc[GX_VA_CLR0];
    const auto& clrArr = g_gxState.arrays[GX_VA_CLR0];
    const auto& clrFmt = g_gxState.vtxFmts[fmt].attrs[GX_VA_CLR0];
    std::string firstClr;
    if (clrDesc != GX_NONE && clrArr.data != nullptr && clrArr.stride != 0) {
      const auto* cb = static_cast<const u8*>(clrArr.data);
      for (u32 v = 0; v < 3; ++v) {
        const u8* rec = cb + static_cast<size_t>(v) * clrArr.stride;
        firstClr += fmt::format(" {:02x}{:02x}{:02x}{:02x}", rec[0], rec[1], rec[2], rec[3]);
      }
    }
    const auto& cc = g_gxState.colorChannelConfig[0];
    const auto& cs = g_gxState.colorChannelState[0];
    std::string lights;
    for (u32 l = 0; l < 8; ++l) {
      const auto& li = g_gxState.lights[l];
      if (li.color.m[0] != 0.f || li.color.m[1] != 0.f || li.color.m[2] != 0.f) {
        lights += fmt::format(" L{}({:.2f},{:.2f},{:.2f})", l, li.color.m[0], li.color.m[1], li.color.m[2]);
      }
    }
    Log.error("GXSHADE array={} clrDesc={} clrType={} clrStride={} clrWS={} clr0..2:{} | lightEn={} matSrc={} "
              "ambSrc={} diffFn={} mat=({:.2f},{:.2f},{:.2f},{:.2f}) amb=({:.2f},{:.2f},{:.2f},{:.2f}) "
              "numChans={} tevStages={} lights:{}",
              array.data, static_cast<u32>(clrDesc), static_cast<u32>(clrFmt.type), clrArr.stride,
              clrArr.wordSwapped, firstClr, cc.lightingEnabled, static_cast<u32>(cc.matSrc),
              static_cast<u32>(cc.ambSrc), static_cast<u32>(cc.diffFn), cs.matColor.m[0], cs.matColor.m[1],
              cs.matColor.m[2], cs.matColor.m[3], cs.ambColor.m[0], cs.ambColor.m[1], cs.ambColor.m[2],
              cs.ambColor.m[3], g_gxState.numChans, g_gxState.numTevStages, lights);
  }

  /* TEV stage configuration.  A surface that renders as one flat colour while its
   * neighbours texture correctly is most likely running a combiner that never
   * samples its texture (no GX_CC_TEXC/TEXA input, or a null texmap/texcoord) and
   * instead emits a constant register. */
  {
    std::string stages;
    for (u32 t = 0; t < g_gxState.numTevStages && t < MaxTevStages; ++t) {
      const auto& st = g_gxState.tevStages[t];
      const auto cc = st.colorPass;
      bool usesTex = cc.a == GX_CC_TEXC || cc.b == GX_CC_TEXC || cc.c == GX_CC_TEXC || cc.d == GX_CC_TEXC ||
                     cc.a == GX_CC_TEXA || cc.b == GX_CC_TEXA || cc.c == GX_CC_TEXA || cc.d == GX_CC_TEXA;
      stages += fmt::format(" [{}]cc({},{},{},{})out{}{} map={} coord={} chan={} kc={}{}", t,
                            static_cast<u32>(cc.a), static_cast<u32>(cc.b), static_cast<u32>(cc.c),
                            static_cast<u32>(cc.d), static_cast<u32>(st.colorOp.outReg),
                            usesTex ? "" : " NOTEX", static_cast<u32>(st.texMapId),
                            static_cast<u32>(st.texCoordId), static_cast<u32>(st.channelId),
                            static_cast<u32>(st.kcSel), st.texMapId == GX_TEXMAP_NULL ? " NULLMAP" : "");
    }
    Log.error("GXSTAGE array={} n={}{}", array.data, g_gxState.numTevStages, stages);
    /* Framebuffer clear colour: if the walls are not geometry at all, the orange
     * is whatever the EFB was cleared to, showing through the gaps. */
    const auto& cc4 = g_gxState.clearColor;
    Log.error("GXCLEAR color=({:.3f},{:.3f},{:.3f},{:.3f}) rgb8=({},{},{}) depth={:#x}", cc4.m[0], cc4.m[1],
              cc4.m[2], cc4.m[3], static_cast<u32>(cc4.m[0] * 255.f + 0.5f), static_cast<u32>(cc4.m[1] * 255.f + 0.5f),
              static_cast<u32>(cc4.m[2] * 255.f + 0.5f), g_gxState.clearDepth);
  }

  /* TEV constant inputs -- the last colour source not yet measured.  Lights,
   * material, ambient and vertex colours are all verified neutral, and textures
   * carry real content, so an orange surface must be getting that colour from a
   * TEV colour/konstant register. */
  {
    std::string regs;
    for (u32 r = 0; r < MaxTevRegs; ++r) {
      const auto& c = g_gxState.colorRegs[r];
      regs += fmt::format(" c{}({:.2f},{:.2f},{:.2f},{:.2f})", r, c.m[0], c.m[1], c.m[2], c.m[3]);
    }
    std::string ks;
    for (u32 k = 0; k < GX_MAX_KCOLOR; ++k) {
      const auto& c = g_gxState.kcolors[k];
      ks += fmt::format(" k{}({:.2f},{:.2f},{:.2f},{:.2f})", k, c.m[0], c.m[1], c.m[2], c.m[3]);
    }
    Log.error("GXTEV array={} stages={}{} |{}", array.data, g_gxState.numTevStages, regs, ks);
  }

  /* Fog is applied at the fragment stage, downstream of every geometry/texture
   * check above, and saturates distant fragments to a single colour -- the exact
   * signature of large smooth-gradient surfaces amid correct near geometry. */
  {
    const auto& f = g_gxState.fog;
    Log.error("GXFOG array={} type={} rangeEnabled={} a={:.4f} b={:.4f} c={:.4f} color=({:.3f},{:.3f},{:.3f},{:.3f}) "
              "rangeCenter={} projNearFar(b={:.4f})",
              array.data, static_cast<u32>(f.type), f.rangeEnabled, f.a, f.b, f.c, f.color.m[0], f.color.m[1],
              f.color.m[2], f.color.m[3], f.rangeCenter, g_gxState.proj[2][3]);
  }

  /* Per-vertex matrix selection: if PNMTXIDX is active, each vertex picks its own
   * pnMtx slot, so a stale/garbage slot warps part of a triangle while the rest
   * stays anchored.  Dump every slot's first row plus its translation. */
  {
    const auto pnDesc = g_gxState.vtxDesc[GX_VA_PNMTXIDX];
    std::string slots;
    for (u32 m = 0; m < 10; ++m) {
      const auto& pm = g_gxState.pnMtx[m].pos;
      float r[12];
      memcpy(r, &pm, sizeof(r));
      bool finite = true;
      for (float f : r) {
        finite = finite && std::isfinite(f);
      }
      slots += fmt::format(" [{}]{}({:.2f},{:.2f},{:.2f}|t {:.1f},{:.1f},{:.1f})", m, finite ? "" : "NONFINITE",
                           r[0], r[1], r[2], r[3], r[7], r[11]);
    }
    Log.error("GXPNMTX array={} pnmtxDesc={} current={} slots:{}", array.data, static_cast<u32>(pnDesc),
              g_gxState.currentPnMtx, slots);
  }

  const char* verdict = okLE > okBE ? "LE" : (okBE > okLE ? "BE" : "tie");
  Log.error("GXDIAG2 array={} stride={} cnt={} type={} frac={} le={} ws={} shaderUses={} plausible(LE={}/{} BE={}/{}) "
            "WANT={} {}{} hex={}",
            array.data, array.stride, cnt, static_cast<u32>(attrFmt.type), attrFmt.frac, array.le, array.wordSwapped,
            shaderLE ? "LE" : "BE", okLE, n, okBE, n, verdict,
            (shaderLE ? okLE : okBE) == 0 ? "MISMATCH" : "ok", sample, hex);
}


/* melee-pc: verify the POS *indices* in a display list, not just the array they
 * select from.  GameCube stores 16-bit indices big-endian and the shader reads
 * them that way; if the bytes were actually little-endian the mesh would keep
 * valid vertex data while triangles connected unrelated vertices.  Reports the
 * index range under both interpretations so the implausible one is obvious. */
void gxdiag_probe_indices(GXVtxFmt fmt, std::span<const u8> vtx, u32 vtxCount, u32 vtxSize) noexcept {
  const auto desc = g_gxState.vtxDesc[GX_VA_POS];
  if (desc != GX_INDEX8 && desc != GX_INDEX16) {
    return;
  }
  const auto& array = g_gxState.arrays[GX_VA_POS];
  static std::set<std::pair<u32, const void*>> seen;
  if (!seen.insert(std::make_pair(gxdiag_gen(), array.data)).second) {
    return;
  }

  // Byte offset of the POS index within each vertex.
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  u32 posOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i < GX_VA_POS; ++i) {
    const auto t = g_gxState.vtxDesc[i];
    if (t == GX_NONE) {
      continue;
    }
    posOffset += t == GX_DIRECT ? 1u : (t == GX_INDEX16 ? 2u : 1u);
  }

  u32 maxBE = 0, maxLE = 0;
  std::string firstBE, firstLE;
  for (u32 v = 0; v < vtxCount; ++v) {
    const size_t off = static_cast<size_t>(v) * vtxSize + posOffset;
    if (off + (desc == GX_INDEX16 ? 2u : 1u) > vtx.size()) {
      break;
    }
    u32 be, le;
    if (desc == GX_INDEX16) {
      be = (static_cast<u32>(vtx[off]) << 8) | vtx[off + 1];
      le = (static_cast<u32>(vtx[off + 1]) << 8) | vtx[off];
    } else {
      be = le = vtx[off];
    }
    maxBE = std::max(maxBE, be);
    maxLE = std::max(maxLE, le);
    if (v < 6) {
      firstBE += fmt::format(" {}", be);
      firstLE += fmt::format(" {}", le);
    }
  }
  /* Direct (in-display-list) CLR0 values.  Meshes that supply colour inline are
   * invisible to any probe that reads the CLR0 *array*, and when the material's
   * texture is I8 (colourless) the vertex colour is the only source of hue. */
  std::string clrOut;
  {
    const auto clrDesc = g_gxState.vtxDesc[GX_VA_CLR0];
    const auto& clrFmt = g_gxState.vtxFmts[fmt].attrs[GX_VA_CLR0];
    if (clrDesc == GX_DIRECT) {
      u32 clrOffset = 0;
      for (int i = GX_VA_PNMTXIDX; i < GX_VA_CLR0; ++i) {
        const auto t = g_gxState.vtxDesc[i];
        if (t == GX_NONE) {
          continue;
        }
        if (t == GX_INDEX16) {
          clrOffset += 2u;
        } else if (t == GX_INDEX8) {
          clrOffset += 1u;
        } else if (i == GX_VA_PNMTXIDX || (i >= GX_VA_TEX0MTXIDX && i <= GX_VA_TEX7MTXIDX)) {
          clrOffset += 1u;
        } else {
          clrOffset += comp_type_size(static_cast<GXAttr>(i), g_gxState.vtxFmts[fmt].attrs[i].type) *
                       comp_cnt_count(static_cast<GXAttr>(i), g_gxState.vtxFmts[fmt].attrs[i].cnt);
        }
      }
      const u32 clrSize = comp_type_size(GX_VA_CLR0, clrFmt.type);
      clrOut = fmt::format(" clrType={} clrOff={} clrSize={} vals:", static_cast<u32>(clrFmt.type), clrOffset,
                           clrSize);
      for (u32 v = 0; v < 4; ++v) {
        const size_t off = static_cast<size_t>(v) * vtxSize + clrOffset;
        if (off + clrSize > vtx.size()) {
          break;
        }
        u32 raw = 0;
        for (u32 b = 0; b < clrSize && b < 4; ++b) {
          raw = (raw << 8) | vtx[off + b];
        }
        if (clrFmt.type == GX_RGBA6 && clrSize == 3) {
          clrOut += fmt::format(" ({},{},{},{})", (raw >> 18) & 0x3F, (raw >> 12) & 0x3F, (raw >> 6) & 0x3F,
                                raw & 0x3F);
        } else {
          clrOut += fmt::format(" {:#0{}x}", raw, clrSize * 2 + 2);
        }
      }
    }
  }

  const u32 posStride = array.stride ? array.stride : 1u;
  Log.error("GXIDX array={} desc={} vtxCount={} vtxSize={} posOffset={} stride={} maxBE={} maxLE={} "
            "byteSpanBE={} byteSpanLE={} firstBE=[{}] firstLE=[{}]{}",
            array.data, static_cast<u32>(desc), vtxCount, vtxSize, posOffset, array.stride, maxBE, maxLE,
            maxBE * posStride, maxLE * posStride, firstBE, firstLE, clrOut);
}


/* melee-pc: skip draws whose POS array matches MELEE_PC_GXSKIP (comma-separated
 * hex addresses).  Identifies which object is painting over the Underground Maze
 * without guessing.  Diagnostic only. */
bool gxdiag_skip_draw() noexcept {
  static const std::set<uintptr_t> skip = [] {
    std::set<uintptr_t> out;
    if (const char* env = getenv("MELEE_PC_GXSKIP")) {
      const char* p = env;
      while (*p != '\0') {
        char* end = nullptr;
        const auto v = strtoull(p, &end, 0);
        if (end == p) {
          break;
        }
        out.insert(static_cast<uintptr_t>(v));
        p = (*end == ',') ? end + 1 : end;
      }
    }
    return out;
  }();
  if (skip.empty()) {
    return false;
  }
  return skip.count(reinterpret_cast<uintptr_t>(g_gxState.arrays[GX_VA_POS].data)) != 0;
}


/* melee-pc: skip any draw whose projected bounding box blankets the whole screen
 * (MELEE_PC_GXSKIPFILL).  Property-based rather than per-array, so it isolates
 * screen-filling geometry without disabling unrelated objects that happen to
 * share a vertex array.  Diagnostic only. */
bool gxdiag_skip_screenfill(GXVtxFmt fmt) noexcept {
  static const bool on = getenv("MELEE_PC_GXSKIPFILL") != nullptr;
  if (!on) {
    return false;
  }
  const auto desc = g_gxState.vtxDesc[GX_VA_POS];
  if (desc != GX_INDEX8 && desc != GX_INDEX16) {
    return false;
  }
  const auto& array = g_gxState.arrays[GX_VA_POS];
  if (array.data == nullptr || array.stride == 0) {
    return false;
  }
  const auto& attrFmt = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
  const bool shaderLE = array.le || array.wordSwapped;
  const u32 cnt = comp_cnt_count(GX_VA_POS, attrFmt.cnt);
  const u32 compSize = comp_type_size(GX_VA_POS, attrFmt.type);
  const auto& pm = g_gxState.pnMtx[g_gxState.currentPnMtx].pos;
  float m[12];
  memcpy(m, &pm, sizeof(m));
  const auto& P = g_gxState.proj;
  float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
  u32 counted = 0;
  for (u32 v = 0; v < 64; ++v) {
    const u8* rec = static_cast<const u8*>(array.data) + static_cast<size_t>(v) * array.stride;
    float o[3]{};
    for (u32 c = 0; c < cnt && c < 3; ++c) {
      o[c] = gxdiag_decode(rec + c * compSize, shaderLE, attrFmt.type, attrFmt.frac);
    }
    if (!gxdiag_plausible(o, std::min(cnt, 3u))) {
      continue;
    }
    const float ex = m[0] * o[0] + m[1] * o[1] + m[2] * o[2] + m[3];
    const float ey = m[4] * o[0] + m[5] * o[1] + m[6] * o[2] + m[7];
    const float ez = m[8] * o[0] + m[9] * o[1] + m[10] * o[2] + m[11];
    const float cw = P[3][0] * ex + P[3][1] * ey + P[3][2] * ez + P[3][3];
    if (cw <= 0.0001f) {
      continue;
    }
    const float nx = (P[0][0] * ex + P[0][1] * ey + P[0][2] * ez + P[0][3]) / cw;
    const float ny = (P[1][0] * ex + P[1][1] * ey + P[1][2] * ez + P[1][3]) / cw;
    if (!std::isfinite(nx) || !std::isfinite(ny)) {
      continue;
    }
    lo[0] = std::min(lo[0], nx);
    hi[0] = std::max(hi[0], nx);
    lo[1] = std::min(lo[1], ny);
    hi[1] = std::max(hi[1], ny);
    ++counted;
  }
  if (counted == 0) {
    return false;
  }
  return (hi[0] - lo[0]) > 2.0f && (hi[1] - lo[1]) > 2.0f;
}


/* melee-pc: skip perspective-space draws that have no texgens (MELEE_PC_GXSKIPFLAT).
 * Such a draw samples a single texel for every pixel, so a textured surface renders
 * as one flat colour.  Restricted to GX_PERSPECTIVE so the orthographic HUD is
 * untouched.  Diagnostic only. */
bool gxdiag_skip_flat() noexcept {
  static const bool on = getenv("MELEE_PC_GXSKIPFLAT") != nullptr;
  if (!on) {
    return false;
  }
  if (g_gxState.projType != GX_PERSPECTIVE) {
    return false;
  }
  const auto desc = g_gxState.vtxDesc[GX_VA_POS];
  if (desc != GX_INDEX8 && desc != GX_INDEX16) {
    return false;
  }
  return g_gxState.numTexGens == 0;
}


/* melee-pc: immediate-mode (GX_DIRECT) draws were invisible to every indexed-POS
 * probe in this file.  Effect/particle geometry is submitted that way, so count it
 * and allow skipping it (MELEE_PC_GXSKIPDIRECT) to test whether the Underground
 * Maze's flat orange surfaces are effects rather than stage geometry.  Diagnostic
 * only. */
void gxdiag_count_direct(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount) noexcept {
  if (g_gxState.vtxDesc[GX_VA_POS] != GX_DIRECT) {
    return;
  }
  static std::set<std::tuple<u32, u32, u32, u32, u32, u32>> seen;
  const auto& attrFmt = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
  if (!seen.insert(std::make_tuple(gxdiag_gen(), static_cast<u32>(prim), static_cast<u32>(g_gxState.projType),
                                   g_gxState.numTexGens, g_gxState.numTevStages,
                                   static_cast<u32>(attrFmt.type)))
           .second) {
    return;
  }
  const auto& tex0 = g_gxState.loadedTextures[0];
  Log.error("GXDIRECT prim={} projType={} vtxCount={} texGens={} tevStages={} posType={} blend={} depthUpd={} "
            "tex0={} texFmt={} {}x{} pos0={}",
            static_cast<u32>(prim), static_cast<u32>(g_gxState.projType), vtxCount, g_gxState.numTexGens,
            g_gxState.numTevStages, static_cast<u32>(attrFmt.type), static_cast<u32>(g_gxState.blendMode),
            g_gxState.depthUpdate, static_cast<const void*>(tex0.data), static_cast<u32>(tex0.mFormat), tex0.mWidth,
            tex0.mHeight, sLastDirectPos);
}

bool gxdiag_skip_direct() noexcept {
  static const bool on = getenv("MELEE_PC_GXSKIPDIRECT") != nullptr;
  if (!on) {
    return false;
  }
  return g_gxState.projType == GX_PERSPECTIVE && g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT;
}


/* melee-pc: skip draws whose bound texture lives outside guest MEM1
 * (MELEE_PC_GXSKIPHOSTTEX).  Every genuine archive texture is a guest pointer
 * (0x8.......); a host-heap texture bound to stage geometry means the material
 * resolved to something other than its archive TObj.  Address-independent, so it
 * survives the host allocator moving between runs.  Diagnostic only. */
bool gxdiag_skip_hosttex() noexcept {
  static const bool on = getenv("MELEE_PC_GXSKIPHOSTTEX") != nullptr;
  if (!on) {
    return false;
  }
  if (g_gxState.projType != GX_PERSPECTIVE) {
    return false;
  }
  const auto* data = g_gxState.loadedTextures[0].data;
  if (data == nullptr) {
    return false;
  }
  return reinterpret_cast<uintptr_t>(data) < 0x80000000ull;
}


/* melee-pc: force fog off (MELEE_PC_NOFOG) as an A/B.  Diagnostic only. */
bool gxdiag_nofog() noexcept {
  static const bool on = getenv("MELEE_PC_NOFOG") != nullptr;
  return on;
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, Reader& reader) noexcept;
static void handle_aurora(Reader& reader) noexcept;

/* TEMPORARY diagnostic (2026-08-11): ring buffer of recently dispatched
 * commands. A FIFO desync is only *detected* at an invalid opcode, which is
 * some distance after the command that actually consumed the wrong number of
 * bytes -- this records enough history to find that command. Dumped by the
 * unknown-opcode path below. */
struct MeleeCmdTrace {
  u32 offset;
  u8 cmd;
  u16 extra; // draw: vertex count
  u16 extra2; // draw: vertex size
};
static constexpr size_t MeleeCmdTraceCap = 64;
static std::array<MeleeCmdTrace, MeleeCmdTraceCap> sMeleeCmdTrace{};
static size_t sMeleeCmdTraceCount = 0;
static void melee_trace_cmd(u32 offset, u8 cmd, u16 extra = 0, u16 extra2 = 0) noexcept {
  sMeleeCmdTrace[sMeleeCmdTraceCount % MeleeCmdTraceCap] = {offset, cmd, extra, extra2};
  ++sMeleeCmdTraceCount;
}

/* TEMPORARY diagnostic (2026-08-11): see note_display_list()'s declaration. */
struct MeleeDlTrace {
  u32 fifoPos;
  u32 nbytes;
  const u8* src;
};
static constexpr size_t MeleeDlTraceCap = 32;
static std::array<MeleeDlTrace, MeleeDlTraceCap> sMeleeDlTrace{};
static size_t sMeleeDlTraceCount = 0;

void note_display_list(u32 fifoPos, u32 nbytes, const void* src) noexcept {
  sMeleeDlTrace[sMeleeDlTraceCount % MeleeDlTraceCap] = {fifoPos, nbytes, static_cast<const u8*>(src)};
  ++sMeleeDlTraceCount;
}

void process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  Reader reader{{data, size}};

  while (!reader.empty()) {
    const u32 cmdOffset = static_cast<u32>(reader.offset());
    const u8 cmd = reader.read<u8>();
    melee_trace_cmd(cmdOffset, cmd);
    /*
     * Dispatch on the exact command byte, not `cmd & GX_OPCODE_MASK`.
     *
     * Only draw commands carry payload in their low bits (the VAT index, 0-7,
     * hence the 0x80-0xBF range); every other GX opcode is a single exact
     * value. Masking made the non-draw cases accept seven bogus aliases each --
     * most damagingly 0x60 and 0x62-0x67, which are not GX opcodes at all but
     * were being run as `GX_LOAD_BP_REG` (0x61) and silently swallowing four
     * operand bytes. Real hardware's command processor rejects them.
     *
     * The practical cost of the old behaviour was diagnostic: a stream desync
     * would be absorbed by a bogus alias and only surface several bytes later
     * at some unrelated byte, so the reported failure point was never where the
     * desync actually began (see pc_port.md entry (35), where the real anomaly
     * was at offset 1624 but the fatal was reported at 1630).
     */
    switch (cmd) {
    case GX_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      handle_bp(reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const u32 arrayType = GX_POS_MTX_ARRAY + (cmd - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      if (array.data == nullptr) {
        Log.error("melee-pc: LOAD_INDX fail arrayType={} srcArrayIdx={} dstAddr={:#x} len={} opcode={:#x} "
                  "readerOffset={} readerSize={}",
                  arrayType, srcArrayIdx, dstAddr, len, cmd, reader.offset(), reader.size());
        for (u32 dbg = 0; dbg < GX_VA_MAX_ATTR; ++dbg) {
          Log.error("melee-pc:   array[{}] data={:#x} size={} stride={}", dbg,
                    reinterpret_cast<uintptr_t>(g_gxState.arrays[dbg].data), g_gxState.arrays[dbg].size,
                    g_gxState.arrays[dbg].stride);
        }
        {
          const u8* base = reader.data();
          size_t curOff = reader.offset();
          size_t start = curOff >= 40 ? curOff - 40 : 0;
          size_t end = std::min(curOff + 16, reader.size());
          std::string hex;
          for (size_t k = start; k < end; ++k) {
            hex += fmt::format("{:02x}{}", base[k], (k == curOff - 5) ? "|" : " ");
          }
          Log.error("melee-pc:   bytes[{}..{}] (cur-5 marked): {}", start, end, hex);
        }
      }
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      // This path reads whole 32-bit words CPU-side, and a 32-bit word swap of a
      // big-endian word is exactly its little-endian form, so GX_ARRAY_WORDSWAPPED_BE
      // needs no separate handling here beyond reading little-endian. (Melee never
      // registers a matrix array, so this is currently unexercised -- but leaving it
      // reading big-endian would be a silent, hard-to-find bug for the first caller
      // that does.)
      const auto srcEndian =
          (array.le || array.wordSwapped) ? std::endian::little : std::endian::big;
      if (!copy_xf_data(dstAddr, srcData, len, srcEndian)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", cmd, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      // Invalidate vertex cache
      break;
    }

    case GX_AURORA: {
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      /* Draw commands are the only ones with payload in their low bits (the VAT
       * index), so they are the only ones matched by range: primitive in bits
       * 3-7, VAT 0-7 in bits 0-2, i.e. 0x80-0xBF. 0xC0 and above is not a
       * primitive and must not be treated as one. The explicit cases above are
       * just the VAT-0 fast paths. */
      if (cmd >= 0x80 && cmd < 0xC0) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        /* TEMPORARY diagnostic (2026-08-11): a desync here means some earlier
         * command consumed the wrong number of bytes, so the useful evidence is
         * the vertex-format state that sizes draw commands, plus a much wider
         * window to find where the stream stopped making sense. */
        {
          const size_t pos = reader.offset();
          const auto& vtxFmt = g_gxState.vtxFmts[g_gxState.lastVtxFmt];
          std::string desc;
          for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
            if (g_gxState.vtxDesc[i] != GX_NONE) {
              desc += fmt::format(" attr{}=type{},cnt{},comp{}", i, static_cast<int>(g_gxState.vtxDesc[i]),
                                  static_cast<int>(vtxFmt.attrs[i].cnt), static_cast<int>(vtxFmt.attrs[i].type));
            }
          }
          Log.error("  lastVtxFmt={} lastVtxSize={} vtxDesc:{}", static_cast<int>(g_gxState.lastVtxFmt),
                    g_gxState.lastVtxSize, desc.empty() ? " (all GX_NONE)" : desc);
          const size_t wideStart = (pos > 320) ? pos - 320 : 0;
          std::string wide;
          for (size_t i = wideStart; i < pos; i++) {
            wide += fmt::format("{}{:02x}", (i % 32 == wideStart % 32) ? "\n    " : " ", data[i]);
          }
          Log.error("  wide dump (pos {}-{}):{}", wideStart, pos - 1, wide);
          std::string trace;
          const size_t traceN = std::min(sMeleeCmdTraceCount, MeleeCmdTraceCap);
          for (size_t i = 0; i < traceN; ++i) {
            const auto& e = sMeleeCmdTrace[(sMeleeCmdTraceCount - traceN + i) % MeleeCmdTraceCap];
            trace += fmt::format("\n    @{:5} cmd={:02x}{}", e.offset, e.cmd,
                                 (e.cmd >= 0x80) ? fmt::format(" nVerts={} vtxSize={}", e.extra, e.extra2) : "");
          }
          Log.error("  last {} commands:{}", traceN, trace);

          /* The decisive comparison: is the byte we choked on what the source
           * display list actually said? `data` is a sub-span of the FIFO buffer,
           * so recover its absolute offset to match against the recorded
           * display-list spans. */
          {
            const u8* fifoBase = get_buffer_data();
            const size_t chunkBase = (fifoBase != nullptr && data >= fifoBase) ? (data - fifoBase) : SIZE_MAX;
            const size_t badAbs = (chunkBase == SIZE_MAX) ? SIZE_MAX : chunkBase + pos - 1;
            bool found = false;
            const size_t dlN = std::min(sMeleeDlTraceCount, MeleeDlTraceCap);
            for (size_t i = 0; i < dlN && badAbs != SIZE_MAX; ++i) {
              const auto& d = sMeleeDlTrace[(sMeleeDlTraceCount - 1 - i) % MeleeDlTraceCap];
              if (d.src == nullptr || badAbs < d.fifoPos || badAbs >= d.fifoPos + d.nbytes) {
                continue;
              }
              found = true;
              const size_t inDl = badAbs - d.fifoPos;
              const size_t from = (inDl > 16) ? inDl - 16 : 0;
              const size_t to = std::min<size_t>(inDl + 16, d.nbytes);
              std::string srcHex;
              std::string fifoHex;
              for (size_t k = from; k < to; ++k) {
                const char* mark = (k == inDl) ? "*" : " ";
                srcHex += fmt::format("{}{:02x}", mark, d.src[k]);
                fifoHex += fmt::format("{}{:02x}", mark, fifoBase[d.fifoPos + k]);
              }
              const bool identical = std::equal(d.src + from, d.src + to, fifoBase + d.fifoPos + from);
              Log.error("  failing byte is inside display list src={} fifoPos={} nbytes={} offsetInDl={}",
                        static_cast<const void*>(d.src), d.fifoPos, d.nbytes, inDl);
              Log.error("    source DL:{}", srcHex);
              Log.error("    in FIFO  :{}", fifoHex);
              Log.error("    -> {}", identical ? "IDENTICAL (bytes came from the archive as-is)"
                                               : "DIFFER (FIFO copy is corrupt)");
              break;
            }
            if (!found) {
              Log.error("  failing byte at abs offset {} is not inside any recorded display list "
                        "(chunkBase={}, {} DLs recorded) -- it was written by immediate-mode GX calls",
                        badAbs, chunkBase, dlN);
            }
          }
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const Reader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);

  /* TEMPORARY diagnostic (2026-08-30): an overrun is the same failure as the
   * unknown-opcode desync -- some earlier command consumed the wrong byte
   * count and left the stream mid-command -- so it needs the same evidence.
   * The command ring is the decisive part: it shows each preceding draw's
   * declared vertex count next to the vertex size the parser sized it with,
   * so the first draw whose (nVerts * vtxSize) disagrees with the distance to
   * the next command is where the desync actually began. */
  {
    std::string desc;
    const auto& vtxFmt = g_gxState.vtxFmts[g_gxState.lastVtxFmt];
    for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
      if (g_gxState.vtxDesc[i] != GX_NONE) {
        desc += fmt::format(" attr{}=type{},cnt{},comp{}", i, static_cast<int>(g_gxState.vtxDesc[i]),
                            static_cast<int>(vtxFmt.attrs[i].cnt), static_cast<int>(vtxFmt.attrs[i].type));
      }
    }
    Log.error("  lastVtxFmt={} lastVtxSize={} vtxDesc:{}", static_cast<int>(g_gxState.lastVtxFmt),
              g_gxState.lastVtxSize, desc.empty() ? " (all GX_NONE)" : desc);

    std::string trace;
    const size_t traceN = std::min(sMeleeCmdTraceCount, MeleeCmdTraceCap);
    for (size_t i = 0; i < traceN; ++i) {
      const auto& e = sMeleeCmdTrace[(sMeleeCmdTraceCount - traceN + i) % MeleeCmdTraceCap];
      trace += fmt::format("\n    @{:5} cmd={:02x}{}", e.offset, e.cmd,
                           (e.cmd >= 0x80) ? fmt::format(" nVerts={} vtxSize={}", e.extra, e.extra2) : "");
    }
    Log.error("  last {} commands:{}", traceN, trace);

    /* Which display list (if any) owns the failing draw -- the overrun means
     * the draw ran past the end of the bytes GXCallDisplayList handed over, so
     * whether the opcode is inside a recorded DL span, and how far from its
     * end, separates "DL length is wrong" from "stream desynced earlier". */
    const u8* fifoBase = get_buffer_data();
    const size_t chunkBase = (fifoBase != nullptr && data >= fifoBase) ? (data - fifoBase) : SIZE_MAX;
    const size_t badAbs = (chunkBase == SIZE_MAX) ? SIZE_MAX : chunkBase + cmdPos;
    bool found = false;
    const size_t dlN = std::min(sMeleeDlTraceCount, MeleeDlTraceCap);
    for (size_t i = 0; i < dlN && badAbs != SIZE_MAX; ++i) {
      const auto& d = sMeleeDlTrace[(sMeleeDlTraceCount - 1 - i) % MeleeDlTraceCap];
      if (d.src == nullptr || badAbs < d.fifoPos || badAbs >= d.fifoPos + d.nbytes) {
        continue;
      }
      found = true;
      const size_t inDl = badAbs - d.fifoPos;
      Log.error("  draw cmd is inside display list src={} fifoPos={} nbytes={} offsetInDl={} bytesLeftInDl={}",
                static_cast<const void*>(d.src), d.fifoPos, d.nbytes, inDl, d.fifoPos + d.nbytes - badAbs);
      /* Compare the display list as the archive holds it against the copy in
       * the FIFO, both at the list's head (where the first draw's vertex count
       * lives) and around the failing byte. A count that differs between the
       * two is a copy/byte-order fault; a count identical in both but still
       * wrong means the archive bytes were mis-unswapped before this point. */
      {
        std::string srcHead;
        std::string fifoHead;
        const size_t headN = std::min<size_t>(32, d.nbytes);
        for (size_t k = 0; k < headN; ++k) {
          srcHead += fmt::format(" {:02x}", d.src[k]);
          fifoHead += fmt::format(" {:02x}", fifoBase[d.fifoPos + k]);
        }
        Log.error("    DL head src :{}", srcHead);
        Log.error("    DL head fifo:{}", fifoHead);
        const size_t from = (inDl > 24) ? inDl - 24 : 0;
        const size_t to = std::min<size_t>(inDl + 24, d.nbytes);
        std::string srcHex;
        std::string fifoHex;
        for (size_t k = from; k < to; ++k) {
          const char* mark = (k == inDl) ? "*" : " ";
          srcHex += fmt::format("{}{:02x}", mark, d.src[k]);
          fifoHex += fmt::format("{}{:02x}", mark, fifoBase[d.fifoPos + k]);
        }
        Log.error("    at fail src :{}", srcHex);
        Log.error("    at fail fifo:{}", fifoHex);
        Log.error("    -> {}", std::equal(d.src + from, d.src + to, fifoBase + d.fifoPos + from)
                                   ? "IDENTICAL (FIFO copy matches archive)"
                                   : "DIFFER (FIFO copy is corrupt)");
      }
      break;
    }
    if (!found) {
      Log.error("  draw cmd at abs offset {} is not inside any recorded display list "
                "(chunkBase={}, {} DLs recorded) -- immediate-mode GX calls",
                badAbs, chunkBase, dlN);
    }
  }

  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      const auto& attrFmt = vtxFmt.attrs[i];
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += (i == GX_VA_NRM && vtxFmt.attrs[i].cnt == GX_NRM_NBT3) ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += (i == GX_VA_NRM && vtxFmt.attrs[i].cnt == GX_NRM_NBT3) ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

/*
 * Upload one vertex array's bytes into this frame's storage buffer.
 *
 * For GX_ARRAY_WORDSWAPPED_BE arrays the source bytes are big-endian GameCube
 * data that a host-side loader already ran a blind 32-bit-word byte swap over
 * (see the GX_ARRAY_* docs in GXGeometry.h). Rather than mutating the caller's
 * memory -- which cannot be done safely, because a GXSetArray call site has no
 * vertex count and therefore no exact array extent, so any fixed-size sweep
 * also flips bytes of whatever unrelated data happens to sit after the array --
 * the swap is un-done here, on the *copy* being handed to the GPU. That copy is
 * private to Aurora, so overshooting the real array end is harmless, and after
 * the transform the data is genuine big-endian, which is what the shader's
 * existing `le = false` fetch path already expects: no shader variant, no
 * pipeline-config change, and therefore no pipeline-cache interaction at all.
 *
 * The 4-byte grouping of the original swap is anchored to absolute addresses,
 * so this is only correct for a 4-byte-aligned array pointer. Vertex arrays out
 * of HSD archives always are; anything else falls back to an untransformed
 * upload rather than silently producing different garbage.
 */
static gfx::Range push_array_storage_uncached(const AttrArray& array) noexcept {
  const auto* src = static_cast<const uint8_t*>(array.data);
  if (!array.wordSwapped || src == nullptr || array.size == 0) {
    return gfx::push_storage(src, array.size);
  }
  if ((reinterpret_cast<uintptr_t>(src) & 3u) != 0) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log.warn("GX_ARRAY_WORDSWAPPED_BE array at {} is not 4-byte aligned; uploading untransformed",
               static_cast<const void*>(src));
    }
    return gfx::push_storage(src, array.size);
  }
  // Command processing is serialized, but this can run either on the FIFO worker
  // thread or inline from fifo::drain(), so keep the scratch buffer thread-local.
  static thread_local std::vector<uint8_t> scratch;
  scratch.resize(array.size);
  const u32 wholeWords = array.size & ~3u;
  for (u32 i = 0; i < wholeWords; i += 4) {
    u32 word;
    memcpy(&word, src + i, sizeof(word));
    word = bswap(word);
    memcpy(scratch.data() + i, &word, sizeof(word));
  }
  // A trailing partial word cannot be un-swapped (its missing bytes were swapped
  // out of range); copy it through so behaviour matches the untransformed path.
  for (u32 i = wholeWords; i < array.size; ++i) {
    scratch[i] = src[i];
  }
  return gfx::push_storage(scratch.data(), scratch.size());
}

static gfx::Range push_array_storage(const AttrArray& array) noexcept {
  const ArrayUploadKey key{array.data, array.le, array.wordSwapped};
  const auto found = sArrayUploadCache.find(key);
  if (found != sArrayUploadCache.end() && found->second.size >= array.size) {
    return found->second;
  }
  const gfx::Range range = push_array_storage_uncached(array);
  sArrayUploadCache.insert_or_assign(key, range);
  return range;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;

  if (gxdiag_enabled()) {
    gxdiag_probe_pos(fmt);
    gxdiag_count_direct(prim, fmt, vtxCount);
  }
  if (gxdiag_skip_draw() || gxdiag_skip_screenfill(fmt) || gxdiag_skip_flat() || gxdiag_skip_direct() ||
      gxdiag_skip_hosttex()) {
    return;
  }

  if (gxdiag_enabled() || gxdiag_drawmax_active()) {
    const int limit = sDrawMax;
    const u32 idx = sFrameDrawIndex++;
    /* MELEE_PC_GXSKIPRANGE=lo:hi drops an inclusive range of per-frame draw
     * indices, so a cluster of related draws (e.g. one effect emitting several
     * quads) can be removed together rather than one at a time. */
    if (sSkipLo >= 0 && static_cast<int>(idx) >= sSkipLo && static_cast<int>(idx) <= sSkipHi) {
      /* MELEE_PC_GXFORCEBLEND: instead of dropping the range, render it with
       * alpha blending enabled.  These draws already carry SRCALPHA/INVSRCALPHA
       * factors but GX_BM_NONE, so if the bug is simply that blending is off,
       * this should make the effect render correctly rather than as an opaque
       * occluding mass. */
      if (getenv("MELEE_PC_GXFORCEBLEND") != nullptr) {
        if (g_gxState.blendMode != GX_BM_BLEND) {
          g_gxState.blendMode = GX_BM_BLEND;
          g_gxState.depthUpdate = false;
          g_gxState.dirty |= DirtyPipeline;
        }
      } else
      /* Log the cluster for a few frames whenever the range changes, so the draws
       * being suppressed can be identified on demand. */
      if (sRangeLogFrames > 0) {
        const auto& pArr = g_gxState.arrays[GX_VA_POS];
        const auto& t0 = g_gxState.loadedTextures[0];
        const auto& pf = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
        Log.error("GXCLUSTER idx={} prim={} vtxCount={} posDesc={} posType={} clrDesc={} texDesc={} "
                  "tevStages={} texGens={} tex0={} texFmt={} {}x{} blend={} src={} dst={} depthUpd={} "
                  "posArray={} projType={}",
                  idx, static_cast<u32>(prim), vtxCount, static_cast<u32>(g_gxState.vtxDesc[GX_VA_POS]),
                  static_cast<u32>(pf.type), static_cast<u32>(g_gxState.vtxDesc[GX_VA_CLR0]),
                  static_cast<u32>(g_gxState.vtxDesc[GX_VA_TEX0]), g_gxState.numTevStages, g_gxState.numTexGens,
                  static_cast<const void*>(t0.data), static_cast<u32>(t0.mFormat), t0.mWidth, t0.mHeight,
                  static_cast<u32>(g_gxState.blendMode), static_cast<u32>(g_gxState.blendFacSrc),
                  static_cast<u32>(g_gxState.blendFacDst), g_gxState.depthUpdate, pArr.data,
                  static_cast<u32>(g_gxState.projType));
      }
      if (getenv("MELEE_PC_GXFORCEBLEND") == nullptr) {
        return;
      }
    }
    /* Skip exactly one draw index (MELEE_PC_GXSKIPIDX) to confirm a single
     * bisected draw is solely responsible, without hiding everything after it. */
    static const int skipIdx = [] {
      const char* e = getenv("MELEE_PC_GXSKIPIDX");
      return e != nullptr ? atoi(e) : -1;
    }();
    if (skipIdx >= 0 && idx == static_cast<u32>(skipIdx)) {
      return;
    }
    if (limit >= 0 && idx >= static_cast<u32>(limit)) {
      return;
    }
    if (limit >= 0 && idx + 1 == static_cast<u32>(limit)) {
      const auto& posArr = g_gxState.arrays[GX_VA_POS];
      const auto& t0 = g_gxState.loadedTextures[0];
      Log.error("GXLAST idx={} posArray={} posDesc={} vtxCount={} prim={} tevStages={} texGens={} tex0={} "
                "texFmt={} {}x{} blend={} depthUpd={}",
                idx, posArr.data, static_cast<u32>(g_gxState.vtxDesc[GX_VA_POS]), vtxCount,
                static_cast<u32>(prim), g_gxState.numTevStages, g_gxState.numTexGens,
                static_cast<const void*>(t0.data), static_cast<u32>(t0.mFormat), t0.mWidth, t0.mHeight,
                static_cast<u32>(g_gxState.blendMode), g_gxState.depthUpdate);
      if (g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && sLastVtxLen != 0) {
        std::string hex;
        for (u32 b = 0; b < sLastVtxLen && b < 96u; ++b) {
          hex += fmt::format("{:02x}{}", sLastVtxBytes[b], (b % sLastVtxSize) == sLastVtxSize - 1 ? " | " : " ");
        }
        std::string pos;
        for (u32 v = 0; v < vtxCount && v < 6; ++v) {
          const u32 off = v * sLastVtxSize;
          if (off + 12 > sLastVtxLen) {
            break;
          }
          float f[3];
          for (u32 c = 0; c < 3; ++c) {
            u32 w;
            memcpy(&w, sLastVtxBytes + off + c * 4, 4);
            w = bswap(w);
            memcpy(&f[c], &w, 4);
          }
          pos += fmt::format(" ({:.1f},{:.1f},{:.1f})", f[0], f[1], f[2]);
        }
        const auto& tm = g_gxState.texMtxs[0];
        float m[12];
        memcpy(m, &tm, sizeof(m));
        std::string desc;
        for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
          const auto t = g_gxState.vtxDesc[i];
          if (t == GX_NONE) {
            continue;
          }
          const auto& af = g_gxState.vtxFmts[fmt].attrs[i];
          desc += fmt::format(" a{}(d={},cnt={},type={},frac={})", i, static_cast<u32>(t),
                              static_cast<u32>(af.cnt), static_cast<u32>(af.type), af.frac);
        }
        Log.error("GXLASTD fmt={} vtxSize={} desc:{}", static_cast<u32>(fmt), sLastVtxSize, desc);
        Log.error("GXLASTV vtxSize={} pos:{} texMtx0=[{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}] hex={}",
                  sLastVtxSize, pos, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], hex);
      }
    }
  }

  DrawImmediateData immediates{.vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0) {
      /* GX display lists use explicit attribute indices; vtxCount is the
       * number of vertices emitted, not the maximum index into every array.
       * Trimming by vtxCount truncates shared/indexed arrays and corrupts
       * geometry (including HUD glyphs). */
      array.cachedRange = push_array_storage(array);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }

  if (gxdiag_nofog() && g_gxState.fog.type != GX_FOG_NONE) {
    g_gxState.fog.type = GX_FOG_NONE;
    g_gxState.dirty |= DirtyPipeline | DirtyUniform;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.config.msaaSamples == gfx::get_sample_count();
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
    cache.pipelineRef = gfx::pipeline_ref(cache.config);
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    resolve_sampled_textures(cache.shaderInfo);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    cache.uniformRange = build_uniform(cache.shaderInfo);
    state.dirty &= ~DirtyUniform;
  }

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  gfx::push_draw_command(DrawData{
      .pipeline = cache.pipelineRef,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = cache.bindGroups,
      .dstAlpha = state.dstAlpha,
  });
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, Reader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  /* TEMPORARY diagnostic (2026-08-11): annotate this draw's ring-buffer entry
   * with the counts that determine how many bytes it consumes -- the usual
   * cause of a desync detected further downstream. */
  if (sMeleeCmdTraceCount > 0) {
    auto& e = sMeleeCmdTrace[(sMeleeCmdTraceCount - 1) % MeleeCmdTraceCap];
    e.extra = vtxCount;
    e.extra2 = static_cast<u16>(vtxSize);
  }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1;

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  const auto vertexData = reader.take(totalVtxBytes);
  if (gxdiag_enabled()) {
    gxdiag_probe_indices(fmt, vertexData, vtxCount, vtxSize);
  }
  if (gxdiag_enabled() && g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT &&
      g_gxState.vtxFmts[fmt].attrs[GX_VA_POS].type == GX_F32 && vertexData.size() >= 12) {
    float f[3];
    for (u32 c = 0; c < 3; ++c) {
      u32 w;
      memcpy(&w, vertexData.data() + c * 4u, 4);
      w = bswap(w);
      memcpy(&f[c], &w, 4);
    }
    snprintf(sLastDirectPos, sizeof(sLastDirectPos), "(%.1f,%.1f,%.1f)", f[0], f[1], f[2]);
  }

  /* Decode positions for draws inside the bisected cluster, so the cluster's real
   * geometry can be seen rather than inferred.  Uses the index push_gx_draw is
   * about to consume, so it lines up with the cluster log. */
  if (gxdiag_enabled() && sSkipLo >= 0 && sRangeLogFrames > 0 &&
      static_cast<int>(sFrameDrawIndex) >= sSkipLo && static_cast<int>(sFrameDrawIndex) <= sSkipHi) {
    const auto& pf = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
    if (g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && pf.type == GX_F32) {
      const u32 pcnt = comp_cnt_count(GX_VA_POS, pf.cnt);
      std::string pos;
      float maxAbs = 0.f;
      u32 nonFinite = 0;
      for (u32 v = 0; v < vtxCount; ++v) {
        const size_t off = static_cast<size_t>(v) * vtxSize;
        if (off + pcnt * 4u > vertexData.size()) {
          break;
        }
        float f[3]{};
        for (u32 c = 0; c < pcnt && c < 3; ++c) {
          u32 w;
          memcpy(&w, vertexData.data() + off + c * 4u, 4);
          w = bswap(w);
          memcpy(&f[c], &w, 4);
          if (!std::isfinite(f[c])) {
            ++nonFinite;
          } else {
            maxAbs = std::max(maxAbs, std::fabs(f[c]));
          }
        }
        if (v < 4) {
          pos += fmt::format(" ({:.1f},{:.1f},{:.1f})", f[0], f[1], f[2]);
        }
      }
      Log.error("GXCLUSTERV idx={} vtxCount={} vtxSize={} maxAbs={:.3g} nonFinite={} first:{}", sFrameDrawIndex,
                vtxCount, vtxSize, maxAbs, nonFinite, pos);
    }
  }

  /* MELEE_PC_GXSKIPBAD: drop any immediate-mode draw whose F32 positions are
   * non-finite or absurdly large.  Draw 166 was only the first of a class, so
   * this tests whether garbage-position geometry as a whole is the bug. */
  if (gxdiag_enabled() && getenv("MELEE_PC_GXSKIPBAD") != nullptr &&
      g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT) {
    const auto& pf = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
    if (pf.type == GX_F32) {
      const u32 pcnt = comp_cnt_count(GX_VA_POS, pf.cnt);
      bool bad = false;
      for (u32 v = 0; v < vtxCount && !bad; ++v) {
        const size_t off = static_cast<size_t>(v) * vtxSize;
        if (off + pcnt * 4u > vertexData.size()) {
          break;
        }
        for (u32 c = 0; c < pcnt; ++c) {
          u32 w;
          memcpy(&w, vertexData.data() + off + c * 4u, 4);
          w = bswap(w);
          float f;
          memcpy(&f, &w, 4);
          if (!std::isfinite(f) || std::fabs(f) > 1.0e6f) {
            bad = true;
            break;
          }
        }
      }
      if (bad) {
        static u32 skipped = 0;
        if (++skipped % 240u == 1u) {
          Log.error("GXSKIPBAD dropped {} garbage-position draws (latest: prim={} vtxCount={})", skipped,
                    static_cast<u32>(prim), vtxCount);
        }
        return;
      }
    }
  }

  /* Log this draw's own vertex bytes when it is the one at the bisection
   * boundary.  sFrameDrawIndex is the index push_gx_draw is about to consume, so
   * the bytes provably belong to that draw -- a buffer stashed for a later reader
   * can be stale when a draw arrives through one of the other submission paths. */
  if (gxdiag_drawmax_active() && sDrawMax >= 0 && sFrameDrawIndex + 1 == static_cast<u32>(sDrawMax)) {
    std::string hex;
    for (size_t b = 0; b < vertexData.size() && b < 96u; ++b) {
      hex += fmt::format("{:02x}{}", vertexData[b], (b % vtxSize) == vtxSize - 1u ? " | " : " ");
    }
    std::string pos;
    const auto& pf = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
    const u32 pcnt = comp_cnt_count(GX_VA_POS, pf.cnt);
    if (g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && pf.type == GX_F32) {
      for (u32 v = 0; v < vtxCount && v < 6; ++v) {
        const size_t off = static_cast<size_t>(v) * vtxSize;
        if (off + pcnt * 4u > vertexData.size()) {
          break;
        }
        pos += " (";
        for (u32 c = 0; c < pcnt; ++c) {
          u32 w;
          memcpy(&w, vertexData.data() + off + c * 4u, 4);
          w = bswap(w);
          float f;
          memcpy(&f, &w, 4);
          pos += fmt::format("{}{:.2f}", c ? "," : "", f);
        }
        pos += ")";
      }
    }
    Log.error("GXBOUND idx={} prim={} vtxCount={} vtxSize={} posCnt={} posType={} pos:{} hex={}",
              sFrameDrawIndex, static_cast<u32>(prim), vtxCount, vtxSize, pcnt, static_cast<u32>(pf.type), pos, hex);
  }
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);

  // Try to merge with previous draw call
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (lastDraw->indexCount != 0) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    ++gfx::g_mergedDrawCallCount;
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
}

static void handle_draw(u8 cmd, Reader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(Reader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const u8 byteOrder = reader.read<u8>();
    // GX_ARRAY_WORDSWAPPED_BE is un-done CPU-side on the uploaded copy, so as far as
    // the shader is concerned the data is plain big-endian.
    const bool le = byteOrder == GX_ARRAY_LE;
    const bool wordSwapped = byteOrder == GX_ARRAY_WORDSWAPPED_BE;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (attrIdx >= GX_POS_MTX_ARRAY) {
      Log.error("melee-pc: GXSetArray attrIdx={} data={:#x} size={} byteOrder={}", attrIdx, arrayAddr, arraySize,
                byteOrder);
    }
    if (array.data != newData || array.size != arraySize || array.le != le || array.wordSwapped != wordSwapped) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.wordSwapped = wordSwapped;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

uint32_t current_draw_index() noexcept { return sFrameDrawIndex; }

void clear_draw_cache() noexcept {
  sFrameDrawIndex = 0;
  gxdiag_drawmax_refresh();
  gxdiag_skiprange_refresh();
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sArrayUploadCache.clear();
  /* Attribute-array storage is frame-local.  Clearing the upload cache alone
   * leaves AttrArray::cachedRange pointing at a range from the previous
   * frame, so subsequent indexed draws skip their upload and fetch whichever
   * vertices happen to occupy that storage now. */
  for (auto& array : g_gxState.arrays) {
    array.cachedRange = {};
  }
}

} // namespace aurora::gx::fifo
