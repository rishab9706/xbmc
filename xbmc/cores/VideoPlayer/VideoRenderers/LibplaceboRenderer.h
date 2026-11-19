/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "system_gl.h"

#include "cores/VideoSettings.h"
#include "FrameBufferObject.h"
#include "RenderInfo.h"
#include "BaseRenderer.h"
#include "utils/Geometry.h"

extern "C" {
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libplacebo/cache.h>
#include <libplacebo/opengl.h>
#include <libplacebo/renderer.h>
}

#include <vector>

class CRenderSystemGL;

#define FIELD_FULL 0
#define FIELD_TOP 1
#define FIELD_BOT 2

class CPlaceboRendererGL : public CBaseRenderer
{
public:
  CPlaceboRendererGL();
  ~CPlaceboRendererGL() override;

  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static bool Register();

  // Player functions
  bool Configure(const VideoPicture &picture, float fps, unsigned int orientation) override;
  bool IsConfigured() override { return m_bConfigured; }
  void AddVideoPicture(const VideoPicture &picture, int index) override;
  void UnInit() override;
  bool Flush(bool saveBuffers) override;
  void SetBufferSize(int numBuffers) override { m_NumYUVBuffers = numBuffers; }
  void ReleaseBuffer(int idx) override;
  void RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  void Update() override;
  CRenderInfo GetRenderInfo() override;
  bool ConfigChanged(const VideoPicture &picture) override;

  // Feature support
  bool SupportsMultiPassRendering() override;
  bool Supports(ERENDERFEATURE feature) const override;
  bool Supports(ESCALINGMETHOD method) const override;

  bool IsGuiLayer() override;
  bool VideoBypassesFramebuffer() override { return false; }

protected:

  struct CYuvPlane
  {
    GLuint id;
    GLuint pbo;
    CRect rect;
    float width;
    float height;
    unsigned texwidth;
    unsigned texheight;
    GLint internalFormat; // GL internal format used for this texture
    //pixels per texel
    unsigned pixpertex_x;
    unsigned pixpertex_y;
    pl_tex wrapped; // cached libplacebo wrapper for `id`
  };

  struct CPictureBuffer
  {
    CPictureBuffer();
    ~CPictureBuffer() = default;

    CYuvPlane fields[MAX_FIELDS][YuvImage::MAX_PLANES];
    YuvImage image;
    GLuint pbo[3]; // one pbo for 3 planes

    CVideoBuffer *videoBuffer;
    bool loaded;

    AVColorPrimaries m_srcPrimaries;
    AVColorSpace m_srcColSpace;
    AVColorTransferCharacteristic m_srcColTransfer;

    int m_srcBits = 8;
    int m_srcTextureBits = 8;
    bool m_srcFullRange;

    bool hasDisplayMetadata = false;
    AVMasteringDisplayMetadata displayMetadata;
    bool hasLightMetadata = false;
    AVContentLightMetadata lightMetadata;
    std::vector<uint8_t> doviMetadata;
  };

  bool Render(unsigned int flags, int renderBuffer);
  void RenderPlacebo(int renderBuffer, int field);
  void ClearBackBuffer();
  void ClearBackBufferQuad();
  void DrawBlackBars();

  virtual AVPixelFormat GetSourceFormat(const VideoPicture& picture) const;

  bool ValidateRenderer();
  virtual bool ValidateRenderTarget();
  void CheckVideoParameters(int index);
  AVColorPrimaries GetSrcPrimaries(AVColorPrimaries srcPrimaries, unsigned int width, unsigned int height);

  // textures
  virtual bool UploadTexture(int index);
  virtual void DeleteTexture(int index);
  virtual bool CreateTexture(int index);

  bool UploadPlanarYUVTexture(int index);
  void DeletePlanarYUVTexture(int index);
  bool CreatePlanarYUVTexture(int index);

  bool UploadNV12Texture(int index);
  void DeleteNV12Texture(int index);
  bool CreateNV12Texture(int index);

  bool UploadPackedYUVTexture(int index);
  void DeletePackedYUVTexture(int index);
  bool CreatePackedYUVTexture(int index);

  // hooks for HwDec Renderer
  virtual bool LoadShadersHook() { return false; }
  virtual bool RenderHook(int idx) { return false; }
  virtual void AfterRenderHook(int idx) {}
  virtual bool CanSaveBuffers() { return true; }

  void CalculateTextureSourceRects(int source, int num_planes);
  GLint GetInternalFormat(GLint format, int bpp);

  void BindPbo(CPictureBuffer& buff);
  void UnBindPbo(CPictureBuffer& buff);
  void LoadPlane(CYuvPlane& plane, int type,
                 unsigned width,  unsigned height,
                 int stride, int bpp, void* data);
  void GetPlaneTextureSize(CYuvPlane& plane);

  pl_tex WrapGLTexture(CYuvPlane& plane);
  void DestroyWrappedTextures(CPictureBuffer& buf);
  bool EnsureRenderTarget();

  bool InitPlacebo();
  void InitCache();

  // Icc Profile
  void InitIcc();
  void CheckIccParams();

  int m_iYUVRenderBuffer = 0;
  int m_NumYUVBuffers = 0;

  bool m_bConfigured = false;
  bool m_bValidated = false;
  GLenum m_textureTarget = GL_TEXTURE_2D;
  CRenderSystemGL *m_renderSystem = nullptr;
  int m_currentField = FIELD_FULL;

  // YUV decoder textures
  // field index 0 is full image, 1 is odd scanlines, 2 is even scanlines
  CPictureBuffer m_buffers[NUM_BUFFERS];

  ESCALINGMETHOD m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  ESCALINGMETHOD m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  bool m_useDithering;
  unsigned int m_ditherDepth;
  bool m_fullRange;
  AVColorPrimaries m_srcPrimaries;
  bool m_toneMap = false;
  ETONEMAPMETHOD m_toneMapMethod = VS_TONEMAPMETHOD_OFF;
  bool m_pboSupported = true;
  bool m_pboUsed = false;
  bool m_streamIsHDR{false};
  bool m_passthroughHDR = false;
  bool m_hdrFboActive{false};

  // Placebo Objects
  pl_opengl m_opengl = NULL;
  pl_gpu m_gpu = NULL;
  pl_renderer m_renderer = NULL;
  pl_log m_log = NULL;
  pl_swapchain m_swapchain = NULL;

  // Persistent shader and 3DLUT caches
  pl_cache m_shaderCache = NULL;
  pl_cache m_iccCache = NULL;
  std::string m_shaderCacheDir;
  std::string m_iccCacheDir;

  // Offscreen render target holding the last rendered video frame
  CFrameBufferObject m_targetFbo;
  pl_tex m_targetTex = NULL;
  int m_targetWidth = 0;
  int m_targetHeight = 0;

  bool m_renderDirty = true;
  int m_lastRenderedBuffer = -1;
  int m_lastRenderedField = -1;
  CRect m_lastRenderDest;
  unsigned int m_renderAlpha = 255;

  // ICC color management
  pl_icc_object m_iccProfile = nullptr;
  pl_icc_params m_iccParams = {};
  std::string m_iccPath = {};
  int m_iccLutSize = 0;
};