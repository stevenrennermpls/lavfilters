/*
 *      Copyright (C) 2011 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 */

#include "stdafx.h"
#include "pixconv_internal.h"

extern "C" {
#include "libavutil/intreadwrite.h"
};

#define ALIGN(x,a) (((x)+(a)-1UL)&~((a)-1UL))

DECLARE_CONV_FUNC_IMPL(convert_generic)
{
  HRESULT hr = S_OK;
  switch (m_OutputPixFmt) {
  case LAVPixFmt_YV12:
    hr = swscale_scale(m_InputPixFmt, PIX_FMT_YUV420P, src, srcStride, dst, width, height, dstStride, lav_pixfmt_desc[m_OutputPixFmt], true);
    break;
  case LAVPixFmt_NV12:
    hr = swscale_scale(m_InputPixFmt, PIX_FMT_NV12, src, srcStride, dst, width, height, dstStride, lav_pixfmt_desc[m_OutputPixFmt]);
    break;
  case LAVPixFmt_YUY2:
    hr = ConvertTo422Packed(src, srcStride, dst, width, height, dstStride);
    break;
  case LAVPixFmt_UYVY:
    hr = ConvertTo422Packed(src, srcStride, dst, width, height, dstStride);
    break;
  case LAVPixFmt_AYUV:
    hr = ConvertToAYUV(src, srcStride, dst, width, height, dstStride);
    break;
  case LAVPixFmt_P010:
    hr = ConvertToPX1X(src, srcStride, dst, width, height, dstStride, 2);
    break;
  case LAVPixFmt_P016:
    hr = ConvertToPX1X(src, srcStride, dst, width, height, dstStride, 2);
    break;
  case LAVPixFmt_P210:
    hr = ConvertToPX1X(src, srcStride, dst, width, height, dstStride, 1);
    break;
  case LAVPixFmt_P216:
    hr = ConvertToPX1X(src, srcStride, dst, width, height, dstStride, 1);
    break;
  case LAVPixFmt_Y410:
    hr = ConvertToY410(src, srcStride, dst, width, height, dstStride);
    break;
  case LAVPixFmt_Y416:
    hr = ConvertToY416(src, srcStride, dst, width, height, dstStride);
    break;
  case LAVPixFmt_RGB32:
    hr = swscale_scale(m_InputPixFmt, PIX_FMT_BGRA, src, srcStride, dst, width, height, dstStride * 4, lav_pixfmt_desc[m_OutputPixFmt]);
    break;
  case LAVPixFmt_RGB24:
    hr = swscale_scale(m_InputPixFmt, PIX_FMT_BGR24, src, srcStride, dst, width, height, dstStride * 3, lav_pixfmt_desc[m_OutputPixFmt]);
    break;
  default:
    ASSERT(0);
    hr = E_FAIL;
    break;
  }

  return S_OK;
}

inline SwsContext *CLAVPixFmtConverter::GetSWSContext(int width, int height, enum PixelFormat srcPix, enum PixelFormat dstPix, int flags)
{
  if (!m_pSwsContext || swsWidth != width || swsHeight != height) {
    // Map full-range formats to their limited-range variants
    // All target formats we have are limited range and we don't want compression
    if (dstPix != PIX_FMT_BGRA && dstPix != PIX_FMT_BGR24) {
      if (srcPix == PIX_FMT_YUVJ420P)
        srcPix = PIX_FMT_YUV420P;
      else if (srcPix == PIX_FMT_YUVJ422P)
        srcPix = PIX_FMT_YUV422P;
      else if (srcPix == PIX_FMT_YUVJ440P)
        srcPix = PIX_FMT_YUV440P;
      else if (srcPix == PIX_FMT_YUVJ444P)
        srcPix = PIX_FMT_YUV444P;
    }

    if (m_pSettings->GetHighQualityPixelFormatConversion()) {
      DbgLog((LOG_TRACE, 10, L"::GetSwsContext(): Activating HQ scaling mode"));
      flags |= (SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND);
    }

    // Get context
    m_pSwsContext = sws_getCachedContext(m_pSwsContext,
                                 width, height, srcPix,
                                 width, height, dstPix,
                                 flags|SWS_PRINT_INFO, NULL, NULL, NULL);

    int *inv_tbl = NULL, *tbl = NULL;
    int srcRange, dstRange, brightness, contrast, saturation;
    int ret = sws_getColorspaceDetails(m_pSwsContext, &inv_tbl, &srcRange, &tbl, &dstRange, &brightness, &contrast, &saturation);
    if (ret >= 0) {
      const int *rgbTbl = NULL;
      if (swsColorSpace != AVCOL_SPC_UNSPECIFIED) {
        rgbTbl = sws_getCoefficients(swsColorSpace);
      } else {
        BOOL isHD = (height >= 720 || width >= 1280);
        rgbTbl = sws_getCoefficients(isHD ? SWS_CS_ITU709 : SWS_CS_ITU601);
      }
      if (swsColorRange != AVCOL_RANGE_UNSPECIFIED) {
        srcRange = dstRange = swsColorRange - 1;
      }
      sws_setColorspaceDetails(m_pSwsContext, rgbTbl, srcRange, tbl, dstRange, brightness, contrast, saturation);
    }
    swsWidth = width;
    swsHeight = height;
  }
  return m_pSwsContext;
}

HRESULT CLAVPixFmtConverter::swscale_scale(enum PixelFormat srcPix, enum PixelFormat dstPix, const uint8_t* const src[], const int srcStride[], BYTE *pOut, int width, int height, int stride, LAVPixFmtDesc pixFmtDesc, bool swapPlanes12)
{
  uint8_t *dst[4];
  int     dstStride[4];
  int     i, ret;

  SwsContext *ctx = GetSWSContext(width, height, srcPix, dstPix, SWS_BILINEAR);
  CheckPointer(m_pSwsContext, E_POINTER);

  memset(dst, 0, sizeof(dst));
  memset(dstStride, 0, sizeof(dstStride));

  dst[0] = pOut;
  dstStride[0] = stride;
  for (i = 1; i < pixFmtDesc.planes; ++i) {
    dst[i] = dst[i-1] + (stride / pixFmtDesc.planeWidth[i-1]) * (height / pixFmtDesc.planeHeight[i-1]);
    dstStride[i] = stride / pixFmtDesc.planeWidth[i];
  }

  if (swapPlanes12) {
    BYTE *tmp = dst[1];
    dst[1] = dst[2];
    dst[2] = tmp;
  }
  ret = sws_scale(ctx, src, srcStride, 0, height, dst, dstStride);

  return S_OK;
}

HRESULT CLAVPixFmtConverter::ConvertTo422Packed(const uint8_t* const src[4], const int srcStride[4], BYTE *pOut, int width, int height, int dstStride)
{
  const BYTE *y = NULL;
  const BYTE *u = NULL;
  const BYTE *v = NULL;
  int line, i;
  int sourceStride = 0;
  BYTE *pTmpBuffer = NULL;

  if (m_InputPixFmt != PIX_FMT_YUV422P && m_InputPixFmt != PIX_FMT_YUVJ422P) {
    uint8_t *dst[4] = {NULL};
    int     dstStride[4] = {0};
    int scaleStride = FFALIGN(width, 32);

    pTmpBuffer = (BYTE *)av_malloc(height * scaleStride * 2);

    dst[0] = pTmpBuffer;
    dst[1] = dst[0] + (height * scaleStride);
    dst[2] = dst[1] + (height * scaleStride / 2);
    dst[3] = NULL;

    dstStride[0] = scaleStride;
    dstStride[1] = scaleStride / 2;
    dstStride[2] = scaleStride / 2;
    dstStride[3] = 0;

    SwsContext *ctx = GetSWSContext(width, height, m_InputPixFmt, PIX_FMT_YUV422P, SWS_FAST_BILINEAR);
    sws_scale(ctx, src, srcStride, 0, height, dst, dstStride);

    y = dst[0];
    u = dst[1];
    v = dst[2];
    sourceStride = scaleStride;
  }  else {
    y = src[0];
    u = src[1];
    v = src[2];
    sourceStride = srcStride[0];
  }

  dstStride <<= 1;

#define YUV422_PACK_YUY2(offset) *idst++ = y[(i+offset) * 2] | (u[i+offset] << 8) | (y[(i+offset) * 2 + 1] << 16) | (v[i+offset] << 24);
#define YUV422_PACK_UYVY(offset) *idst++ = u[i+offset] | (y[(i+offset) * 2] << 8) | (v[i+offset] << 16) | (y[(i+offset) * 2 + 1] << 24);

  BYTE *out = pOut;
  int halfwidth = width >> 1;
  int halfstride = sourceStride >> 1;

  if (m_OutputPixFmt == LAVPixFmt_YUY2) {
    for (line = 0; line < height; ++line) {
      uint32_t *idst = (uint32_t *)out;
      for (i = 0; i < (halfwidth - 7); i+=8) {
        YUV422_PACK_YUY2(0)
        YUV422_PACK_YUY2(1)
        YUV422_PACK_YUY2(2)
        YUV422_PACK_YUY2(3)
        YUV422_PACK_YUY2(4)
        YUV422_PACK_YUY2(5)
        YUV422_PACK_YUY2(6)
        YUV422_PACK_YUY2(7)
      }
      for(; i < halfwidth; ++i) {
        YUV422_PACK_YUY2(0)
      }
      y += sourceStride;
      u += halfstride;
      v += halfstride;
      out += dstStride;
    }
  } else {
    for (line = 0; line < height; ++line) {
      uint32_t *idst = (uint32_t *)out;
      for (i = 0; i < (halfwidth - 7); i+=8) {
        YUV422_PACK_UYVY(0)
        YUV422_PACK_UYVY(1)
        YUV422_PACK_UYVY(2)
        YUV422_PACK_UYVY(3)
        YUV422_PACK_UYVY(4)
        YUV422_PACK_UYVY(5)
        YUV422_PACK_UYVY(6)
        YUV422_PACK_UYVY(7)
      }
      for(; i < halfwidth; ++i) {
        YUV422_PACK_UYVY(0)
      }
      y += sourceStride;
      u += halfstride;
      v += halfstride;
      out += dstStride;
    }
  }

  av_freep(&pTmpBuffer);

  return S_OK;
}

HRESULT CLAVPixFmtConverter::ConvertToAYUV(const uint8_t* const src[4], const int srcStride[4], BYTE *pOut, int width, int height, int dstStride)
{
  const BYTE *y = NULL;
  const BYTE *u = NULL;
  const BYTE *v = NULL;
  int line, i = 0;
  int sourceStride = 0;
  BYTE *pTmpBuffer = NULL;

  if (m_InputPixFmt != PIX_FMT_YUV444P && m_InputPixFmt != PIX_FMT_YUVJ444P) {
    uint8_t *dst[4] = {NULL};
    int     swStride[4] = {0};
    int scaleStride = FFALIGN(dstStride, 32);

    pTmpBuffer = (BYTE *)av_malloc(height * scaleStride * 3);

    dst[0] = pTmpBuffer;
    dst[1] = dst[0] + (height * scaleStride);
    dst[2] = dst[1] + (height * scaleStride);
    dst[3] = NULL;
    swStride[0] = scaleStride;
    swStride[1] = scaleStride;
    swStride[2] = scaleStride;
    swStride[3] = 0;

    SwsContext *ctx = GetSWSContext(width, height, m_InputPixFmt, PIX_FMT_YUV444P, SWS_POINT);
    sws_scale(ctx, src, srcStride, 0, height, dst, swStride);

    y = dst[0];
    u = dst[1];
    v = dst[2];
    sourceStride = scaleStride;
  } else {
    y = src[0];
    u = src[1];
    v = src[2];
    sourceStride = srcStride[0];
  }

#define YUV444_PACK_AYUV(offset) *idst++ = v[i+offset] | (u[i+offset] << 8) | (y[i+offset] << 16) | (0xff << 24);

  BYTE *out = pOut;
  for (line = 0; line < height; ++line) {
    int32_t *idst = (int32_t *)out;
    for (i = 0; i < (width-7); i+=8) {
      YUV444_PACK_AYUV(0)
      YUV444_PACK_AYUV(1)
      YUV444_PACK_AYUV(2)
      YUV444_PACK_AYUV(3)
      YUV444_PACK_AYUV(4)
      YUV444_PACK_AYUV(5)
      YUV444_PACK_AYUV(6)
      YUV444_PACK_AYUV(7)
    }
    for (; i < width; ++i) {
      YUV444_PACK_AYUV(0)
    }
    y += sourceStride;
    u += sourceStride;
    v += sourceStride;
    out += dstStride << 2;
  }

  av_freep(&pTmpBuffer);

  return S_OK;
}

HRESULT CLAVPixFmtConverter::ConvertToPX1X(const uint8_t* const src[4], const int srcStride[4], BYTE *pOut, int width, int height, int dstStride, int chromaVertical)
{
  const BYTE *y = NULL;
  const BYTE *u = NULL;
  const BYTE *v = NULL;
  int line, i = 0;
  int sourceStride = 0;

  int shift = 0;
  BOOL bBigEndian = FALSE;

  // Stride needs to be doubled for 16-bit per pixel
  dstStride <<= 1;

  BYTE *pTmpBuffer = NULL;

  if ((chromaVertical == 1 && m_InputPixFmt != PIX_FMT_YUV422P16LE && m_InputPixFmt != PIX_FMT_YUV422P16BE && m_InputPixFmt != PIX_FMT_YUV422P10LE && m_InputPixFmt != PIX_FMT_YUV422P10BE)
    || (chromaVertical == 2 && m_InputPixFmt != PIX_FMT_YUV420P16LE && m_InputPixFmt != PIX_FMT_YUV420P16BE && m_InputPixFmt != PIX_FMT_YUV420P10LE && m_InputPixFmt != PIX_FMT_YUV420P10BE
        && m_InputPixFmt != PIX_FMT_YUV420P9LE && m_InputPixFmt != PIX_FMT_YUV420P9BE)) {
    uint8_t *dst[4] = {NULL};
    int     dstStride[4] = {0};
    int scaleStride = FFALIGN(width, 32) * 2;

    pTmpBuffer = (BYTE *)av_malloc(height * scaleStride * 2);

    dst[0] = pTmpBuffer;
    dst[1] = dst[0] + (height * scaleStride);
    dst[2] = dst[1] + ((height / chromaVertical) * (scaleStride / 2));
    dst[3] = NULL;
    dstStride[0] = scaleStride;
    dstStride[1] = scaleStride / 2;
    dstStride[2] = scaleStride / 2;
    dstStride[3] = 0;

    SwsContext *ctx = GetSWSContext(width, height, m_InputPixFmt, chromaVertical == 1 ? PIX_FMT_YUV422P16LE : PIX_FMT_YUV420P16LE, SWS_POINT);
    sws_scale(ctx, src, srcStride, 0, height, dst, dstStride);

    y = dst[0];
    u = dst[1];
    v = dst[2];
    sourceStride = scaleStride;
  } else {
    y = src[0];
    u = src[1];
    v = src[2];
    sourceStride = srcStride[0];

    if (m_InputPixFmt == PIX_FMT_YUV422P10LE || m_InputPixFmt == PIX_FMT_YUV422P10BE || m_InputPixFmt == PIX_FMT_YUV420P10LE || m_InputPixFmt == PIX_FMT_YUV420P10BE)
      shift = 6;
    else if (m_InputPixFmt == PIX_FMT_YUV420P9LE || m_InputPixFmt == PIX_FMT_YUV420P9BE)
      shift = 7;

    bBigEndian = (m_InputPixFmt == PIX_FMT_YUV422P16BE || m_InputPixFmt == PIX_FMT_YUV422P10BE || m_InputPixFmt == PIX_FMT_YUV420P16BE || m_InputPixFmt == PIX_FMT_YUV420P10BE || m_InputPixFmt == PIX_FMT_YUV420P9BE);
  }

  // copy Y
  BYTE *pLineOut = pOut;
  const BYTE *pLineIn = y;
  for (line = 0; line < height; ++line) {
    if (shift == 0 && !bBigEndian) {
      memcpy(pLineOut, pLineIn, width * 2);
    } else {
      const int16_t *yc = (int16_t *)pLineIn;
      int16_t *idst = (int16_t *)pLineOut;
      for (i = 0; i < width; ++i) {
        int32_t yv;
        if (bBigEndian) yv = AV_RB16(yc+i); else yv = AV_RL16(yc+i);
        if (shift) yv <<= shift;
        *idst++ = yv;
      }
    }
    pLineOut += dstStride;
    pLineIn += sourceStride;
  }

  sourceStride >>= 2;

  // Merge U/V
  BYTE *out = pLineOut;
  const int16_t *uc = (int16_t *)u;
  const int16_t *vc = (int16_t *)v;
  for (line = 0; line < height/chromaVertical; ++line) {
    int32_t *idst = (int32_t *)out;
    for (i = 0; i < width/2; ++i) {
      int32_t uv, vv;
      if (bBigEndian) {
        uv = AV_RB16(uc+i);
        vv = AV_RB16(vc+i);
      } else {
        uv = AV_RL16(uc+i);
        vv = AV_RL16(vc+i);
      }
      if (shift) {
        uv <<= shift;
        vv <<= shift;
      }
      *idst++ = uv | (vv << 16);
    }
    uc += sourceStride;
    vc += sourceStride;
    out += dstStride;
  }

  av_freep(&pTmpBuffer);

  return S_OK;
}

#define YUV444_PACKED_LOOP_HEAD(width, height, y, u, v, out) \
  for (int line = 0; line < height; ++line) { \
    int32_t *idst = (int32_t *)out; \
    for(int i = 0; i < width; ++i) { \
      int32_t yv, uv, vv;

#define YUV444_PACKED_LOOP_HEAD_LE(width, height, y, u, v, out) \
  YUV444_PACKED_LOOP_HEAD(width, height, y, u, v, out) \
    yv = AV_RL16(y+i); uv = AV_RL16(u+i); vv = AV_RL16(v+i);

#define YUV444_PACKED_LOOP_HEAD_BE(width, height, y, u, v, out) \
  YUV444_PACKED_LOOP_HEAD(width, height, y, u, v, out) \
    yv = AV_RB16(y+i); uv = AV_RB16(u+i); vv = AV_RB16(v+i);

#define YUV444_PACKED_LOOP_END(y, u, v, out, srcStride, dstStride) \
    } \
    y += srcStride; \
    u += srcStride; \
    v += srcStride; \
    out += dstStride; \
  }

HRESULT CLAVPixFmtConverter::ConvertToY410(const uint8_t* const src[4], const int srcStride[4], BYTE *pOut, int width, int height, int dstStride)
{
  const int16_t *y = NULL;
  const int16_t *u = NULL;
  const int16_t *v = NULL;
  int sourceStride = 0;
  bool bBigEndian = false, b9Bit = false;

  BYTE *pTmpBuffer = NULL;

  if (m_InputPixFmt != PIX_FMT_YUV444P10BE && m_InputPixFmt != PIX_FMT_YUV444P10LE && m_InputPixFmt != PIX_FMT_YUV444P9BE && m_InputPixFmt != PIX_FMT_YUV444P9LE) {
    uint8_t *dst[4] = {NULL};
    int     dstStride[4] = {0};
    int scaleStride = FFALIGN(width, 32);

    pTmpBuffer = (BYTE *)av_malloc(height * scaleStride * 6);

    dst[0] = pTmpBuffer;
    dst[1] = dst[0] + (height * scaleStride * 2);
    dst[2] = dst[1] + (height * scaleStride * 2);
    dst[3] = NULL;
    dstStride[0] = scaleStride * 2;
    dstStride[1] = scaleStride * 2;
    dstStride[2] = scaleStride * 2;
    dstStride[3] = 0;

    SwsContext *ctx = GetSWSContext(width, height, m_InputPixFmt, PIX_FMT_YUV444P10LE, SWS_POINT);
    sws_scale(ctx, src, srcStride, 0, height, dst, dstStride);

    y = (int16_t *)dst[0];
    u = (int16_t *)dst[1];
    v = (int16_t *)dst[2];
    sourceStride = scaleStride;
  } else {
    y = (int16_t *)src[0];
    u = (int16_t *)src[1];
    v = (int16_t *)src[2];
    sourceStride = srcStride[0] / 2;

    bBigEndian = (m_InputPixFmt == PIX_FMT_YUV444P10BE || m_InputPixFmt == PIX_FMT_YUV444P9BE);
    b9Bit = (m_InputPixFmt == PIX_FMT_YUV444P9BE || m_InputPixFmt == PIX_FMT_YUV444P9LE);
  }

  // 32-bit per pixel
  dstStride *= 4;

#define YUV444_Y410_PACK \
  *idst++ = (uv & 0x3FF) | ((yv & 0x3FF) << 10) | ((vv & 0x3FF) << 20) | (3 << 30);

  BYTE *out = pOut;
  if (bBigEndian) {
    YUV444_PACKED_LOOP_HEAD_BE(width, height, y, u, v, out)
      if (b9Bit) {
        yv <<= 1;
        uv <<= 1;
        vv <<= 1;
      }
      YUV444_Y410_PACK
    YUV444_PACKED_LOOP_END(y, u, v, out, sourceStride, dstStride)
  } else {
    YUV444_PACKED_LOOP_HEAD_LE(width, height, y, u, v, out)
      if (b9Bit) {
        yv <<= 1;
        uv <<= 1;
        vv <<= 1;
      }
      YUV444_Y410_PACK
    YUV444_PACKED_LOOP_END(y, u, v, out, sourceStride, dstStride)
  }

  av_freep(&pTmpBuffer);

  return S_OK;
}

HRESULT CLAVPixFmtConverter::ConvertToY416(const uint8_t* const src[4], const int srcStride[4], BYTE *pOut, int width, int height, int dstStride)
{
  const int16_t *y = NULL;
  const int16_t *u = NULL;
  const int16_t *v = NULL;
  int sourceStride = 0;
  bool bBigEndian = false;

  BYTE *pTmpBuffer = NULL;

  if (m_InputPixFmt != PIX_FMT_YUV444P16BE && m_InputPixFmt != PIX_FMT_YUV444P16LE) {
    uint8_t *dst[4] = {NULL};
    int     dstStride[4] = {0};
    int scaleStride = FFALIGN(width, 32);

    pTmpBuffer = (BYTE *)av_malloc(height * scaleStride * 6);

    dst[0] = pTmpBuffer;
    dst[1] = dst[0] + (height * scaleStride * 2);
    dst[2] = dst[1] + (height * scaleStride * 2);
    dst[3] = NULL;
    dstStride[0] = scaleStride * 2;
    dstStride[1] = scaleStride * 2;
    dstStride[2] = scaleStride * 2;
    dstStride[3] = 0;

    SwsContext *ctx = GetSWSContext(width, height, m_InputPixFmt, PIX_FMT_YUV444P16LE, SWS_POINT);
    sws_scale(ctx, src, srcStride, 0, height, dst, dstStride);

    y = (int16_t *)dst[0];
    u = (int16_t *)dst[1];
    v = (int16_t *)dst[2];
    sourceStride = scaleStride;
  } else {
    y = (int16_t *)src[0];
    u = (int16_t *)src[1];
    v = (int16_t *)src[2];
    sourceStride = srcStride[0] / 2;

    bBigEndian = (m_InputPixFmt == PIX_FMT_YUV444P16BE);
  }

  // 64-bit per pixel
  dstStride <<= 3;

#define YUV444_Y416_PACK \
  *idst++ = 0xFFFF | (vv << 16); \
  *idst++ = yv | (uv << 16);

  BYTE *out = pOut;
  if (bBigEndian) {
    YUV444_PACKED_LOOP_HEAD_BE(width, height, y, u, v, out)
      YUV444_Y416_PACK
    YUV444_PACKED_LOOP_END(y, u, v, out, sourceStride, dstStride)
  } else {
    YUV444_PACKED_LOOP_HEAD_LE(width, height, y, u, v, out)
      YUV444_Y416_PACK
    YUV444_PACKED_LOOP_END(y, u, v, out, sourceStride, dstStride)
  }

  av_freep(&pTmpBuffer);

  return S_OK;
}