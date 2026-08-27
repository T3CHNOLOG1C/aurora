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

#include <cstdint>
#include <span>
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

void clear_draw_cache() noexcept {
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sArrayUploadCache.clear();
}

} // namespace aurora::gx::fifo
