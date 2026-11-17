/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererPlaceboVAAPI.h"

#include "../RenderFactory.h"
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#include "utils/GLUtils.h"
#include "utils/log.h"

#include <libavutil/pixfmt.h>
#include <memory>

using namespace VAAPI;

IVaapiWinSystem* CRendererPlaceboVAAPI::m_pWinSystem = nullptr;

namespace
{
AVPixelFormat PixFmtFromFourcc(std::int32_t fourcc)
{
  for (const auto& entry : kVaFormatTable)
  {
    if (entry.vaFourcc == static_cast<std::uint32_t>(fourcc))
      return entry.pixFmt;
  }
  return AV_PIX_FMT_NONE;
}
} // namespace

AVPixelFormat CRendererPlaceboVAAPI::GetSourceFormat(const VideoPicture& picture) const
{
  auto* pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
  if (!pic)
    return AV_PIX_FMT_NONE;

  // VAAPI buffers report AV_PIX_FMT_NONE; use the surface fourcc for hardware
  // pictures and the frame format for software post-processing output.
  if (pic->procPic.videoSurface != VA_INVALID_ID)
    return PixFmtFromFourcc(pic->procPic.fourcc);

  return pic->avFrame ? static_cast<AVPixelFormat>(pic->avFrame->format) : AV_PIX_FMT_NONE;
}

CBaseRenderer* CRendererPlaceboVAAPI::Create(CVideoBuffer* buffer)
{
  CVaapiRenderPicture *vb = dynamic_cast<CVaapiRenderPicture*>(buffer);
  if (vb)
    return new CRendererPlaceboVAAPI();

  return nullptr;
}

void CRendererPlaceboVAAPI::Register(IVaapiWinSystem* winSystem,
                                VADisplay vaDpy,
                                EGLDisplay eglDisplay,
                                bool& general,
                                bool& deepColor)
{
  general = deepColor = false;

  int major_version, minor_version;
  if (vaInitialize(vaDpy, &major_version, &minor_version) != VA_STATUS_SUCCESS)
  {
    vaTerminate(vaDpy);
    return;
  }

  // Probe importable surface formats via vaExportSurfaceHandle.
  CCapabilities& caps = CDecoder::GetCaps();
  CVaapi2Texture::TestInteropFormats(vaDpy, eglDisplay, caps);

  CLog::Log(LOGDEBUG, "VAAPI EGL interop: {}", caps.ToString());

  // Bool out-params are views over caps for the OptionalsReg / WinSystem
  // boundary that still expresses capability as the general/deepColor pair.
  general = caps.Supports(AV_PIX_FMT_NV12);
  deepColor = caps.Supports(AV_PIX_FMT_P010);

  vaTerminate(vaDpy);

  if (general)
  {
    VIDEOPLAYER::CRendererFactory::RegisterRenderer("vaapi", CRendererPlaceboVAAPI::Create);
    m_pWinSystem = winSystem;
  }
}

CRendererPlaceboVAAPI::CRendererPlaceboVAAPI() = default;

CRendererPlaceboVAAPI::~CRendererPlaceboVAAPI()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }
  // renderer destruction runs on the render thread with the GL context current
  m_texturePool.ReleaseAll();
}

bool CRendererPlaceboVAAPI::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
  if (!pic)
    return false;

  if (pic->procPic.videoSurface == VA_INVALID_ID)
  {
    m_isVAAPIBuffer = false;
  }
  else
  {
    m_isVAAPIBuffer = true;

    InteropInfo interop;
    interop.textureTarget = GL_TEXTURE_2D;
    interop.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    interop.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    interop.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    interop.eglDisplay = CRendererPlaceboVAAPI::m_pWinSystem->GetEGLDisplay();

    m_texturePool.ReleaseAll();
    m_texturePool.Init(interop);
    for (auto &tex : m_vaapiTextures)
      tex = nullptr;
  }

  for (auto &fence : m_fences)
  {
    fence = {};
  }

  return CPlaceboRendererGL::Configure(picture, fps, orientation);
}

bool CRendererPlaceboVAAPI::Flush(bool saveBuffers)
{
  for (auto &vaapiTexture : m_vaapiTextures)
  {
    if (m_isVAAPIBuffer)
      vaapiTexture = nullptr;
  }
  return CPlaceboRendererGL::Flush(saveBuffers);
}

bool CRendererPlaceboVAAPI::ConfigChanged(const VideoPicture& picture)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
  if ((pic->procPic.videoSurface != VA_INVALID_ID && !m_isVAAPIBuffer) ||
      (pic->procPic.videoSurface == VA_INVALID_ID && m_isVAAPIBuffer))
    return true;

  return false;
}

bool CRendererPlaceboVAAPI::Supports(ERENDERFEATURE feature) const
{
  return CPlaceboRendererGL::Supports(feature);
}

bool CRendererPlaceboVAAPI::Supports(ESCALINGMETHOD method) const
{
  return CPlaceboRendererGL::Supports(method);
}

bool CRendererPlaceboVAAPI::CreateTexture(int index)
{
  if (!m_isVAAPIBuffer)
  {
    return CreateNV12Texture(index);
  }

  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = buf.image;
  CYuvPlane (&planes)[YuvImage::MAX_PLANES] = buf.fields[0];

  DeleteTexture(index);

  memset(&im, 0, sizeof(im));
  memset(&planes, 0, sizeof(CYuvPlane[YuvImage::MAX_PLANES]));
  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  planes[0].id = 1;

  return true;
}

void CRendererPlaceboVAAPI::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  if (!m_isVAAPIBuffer)
  {
    DeleteNV12Texture(index);
    return;
  }

  CPictureBuffer &buf = m_buffers[index];
  DestroyWrappedTextures(buf);
  buf.fields[FIELD_FULL][0].id = 0;
  buf.fields[FIELD_FULL][1].id = 0;
  buf.fields[FIELD_FULL][2].id = 0;
}

bool CRendererPlaceboVAAPI::UploadTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(buf.videoBuffer);

  if (!pic || !pic->valid)
  {
    return false;
  }

  if (!m_isVAAPIBuffer)
  {
    if (!m_buffers[index].loaded)
    {
      YuvImage &dst = m_buffers[index].image;
      YuvImage src;
      pic->GetPlanes(src.plane);
      pic->GetStrides(src.stride);
      UnBindPbo(m_buffers[index]);
      CVideoBuffer::CopyNV12Picture(&dst, &src);
      BindPbo(m_buffers[index]);
    }
    CalculateTextureSourceRects(index, 3);
    return UploadNV12Texture(index);
  }

  m_vaapiTextures[index] = m_texturePool.Get(pic, m_vaapiTextures);
  if (!m_vaapiTextures[index])
    return false;

  const YuvImage& im = buf.image;
  CYuvPlane (&planes)[3] = buf.fields[FIELD_FULL];

  const GLuint ids[2] = {m_vaapiTextures[index]->GetTextureY(),
                         m_vaapiTextures[index]->GetTextureVU()};

  // Wrappers cache the texture id they were created for; the pool rotates
  // textures with the surfaces, so drop them whenever the ids change.
  if (planes[0].id != ids[0] || planes[1].id != ids[1])
    DestroyWrappedTextures(buf);

  auto size = m_vaapiTextures[index]->GetTextureSize();
  planes[0].texwidth  = size.Width();
  planes[0].texheight = size.Height();

  planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
  planes[1].texheight = planes[0].texheight >> im.cshift_y;
  planes[2].texwidth  = planes[1].texwidth;
  planes[2].texheight = planes[1].texheight;

  planes[0].id = ids[0];
  planes[1].id = ids[1];
  planes[2].id = ids[1];

  for (int p = 0; p < 3; p++)
  {
    planes[p].pixpertex_x = 1;
    planes[p].pixpertex_y = 1;
  }

  for (int p=0; p<2; p++)
  {
    glBindTexture(m_textureTarget, planes[p].id);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // pl_opengl_wrap() asserts on a zero iformat, and needs the real internal
    // format of the imported EGL image (GL_R8/GL_R16, GL_RG8/GL_RG16).
    glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &planes[p].internalFormat);

    glBindTexture(m_textureTarget, 0);
    VerifyGLState();
  }

  planes[2].internalFormat = planes[1].internalFormat;

  CalculateTextureSourceRects(index, 3);
  m_renderDirty = true;
  return true;
}

void CRendererPlaceboVAAPI::AfterRenderHook(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  m_fences[idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool CRendererPlaceboVAAPI::LoadShadersHook()
{
  return false;
}

bool CRendererPlaceboVAAPI::RenderHook(int idx)
{
  return false;
}

bool CRendererPlaceboVAAPI::NeedBuffer(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    GLint state;
    GLsizei length;
    glGetSynciv(m_fences[idx], GL_SYNC_STATUS, 1, &length, &state);
    if (state == GL_SIGNALED)
    {
      glDeleteSync(m_fences[idx]);
      m_fences[idx] = {};
    }
    else
    {
      return true;
    }
  }

  return false;
}

void CRendererPlaceboVAAPI::ReleaseBuffer(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  if (m_isVAAPIBuffer)
    m_vaapiTextures[idx] = nullptr;

  CPlaceboRendererGL::ReleaseBuffer(idx);
}