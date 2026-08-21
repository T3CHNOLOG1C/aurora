#ifndef DOLPHIN_GXGEOMETRY_H
#define DOLPHIN_GXGEOMETRY_H

#include <dolphin/gx/GXEnum.h>

#ifdef __cplusplus
extern "C" {
#endif

void GXSetVtxDesc(GXAttr attr, GXAttrType type);
void GXSetVtxDescv(GXVtxDescList* list);
void GXClearVtxDesc(void);
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list);
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
void GXSetNumTexGens(u8 nTexGens);
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
#ifdef TARGET_PC
/**
 * Aurora extension: pass as GXBegin's vertex count to have it derived automatically from
 * the number of bytes written before GXEnd. Not supported while recording a display list.
 */
#define GX_AUTO 0xFFFF

/**
 * Aurora extension: begin an indexed triangle draw. The caller writes nverts vertices
 * then calls GXEnd. Indices are copied immediately on begin.
 */
void GXBeginIndexed(GXVtxFmt vtxfmt, u16 nverts, const u16* indices, u32 nindices);
#endif
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 postmtx);
void GXSetLineWidth(u8 width, GXTexOffset texOffsets);
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets);
void GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable);
#ifdef TARGET_PC
/*
 * Byte order of the raw bytes a vertex array points at.
 *
 * GX_ARRAY_BE / GX_ARRAY_LE are the original two states -- this parameter
 * used to be a plain `bool le`, and both spellings still convert correctly.
 *
 * GX_ARRAY_WORDSWAPPED_BE describes big-endian GameCube array data that a
 * host-side loader has already run a blind 32-bit-word byte swap over: the
 * usual result of unswapping a whole archive/DAT buffer on a little-endian
 * host, which is right for scalar struct fields but wrong for opaque
 * per-vertex byte streams. Aurora un-does that swap on the copy it uploads
 * to the GPU (never on the caller's memory), so component reads of any width
 * behave exactly as they would against untouched big-endian data. Requires a
 * 4-byte-aligned `data` pointer, since the word grouping is anchored to
 * absolute addresses; unaligned pointers fall back to GX_ARRAY_BE with a
 * one-time warning.
 */
#define GX_ARRAY_BE 0u
#define GX_ARRAY_LE 1u
#define GX_ARRAY_WORDSWAPPED_BE 2u
void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, u8 byteOrder);
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (size), (stride), (le))
#else
void GXSetArray(GXAttr attr, const void* data, u8 stride);
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (stride))
#endif
void GXInvalidateVtxCache(void);

static inline void GXSetTexCoordGen(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx) {
  GXSetTexCoordGen2(dst_coord, func, src_param, mtx, GX_FALSE, GX_PTIDENTITY);
}

#ifdef __cplusplus
}
#endif

#endif
