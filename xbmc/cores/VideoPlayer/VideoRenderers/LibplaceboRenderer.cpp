#include "LibplaceboRenderer.h"

#include "FileItemList.h"
#include "RenderFactory.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "rendering/GLExtensions.h"
#include "rendering/gl/RenderSystemGL.h"
#include "utils/GLUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "settings/SettingsComponent.h"
#include "settings/Settings.h"
#include "ColorManager.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define PBO_OFFSET 16

#ifdef TARGET_DARWIN_OSX
#include "platform/darwin/osx/CocoaInterface.h"
#endif

#if defined(TARGET_DARWIN_OSX)
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/CGLIOSurface.h>
#endif

extern "C" {
  #include <libplacebo/utils/libav.h>
}

namespace
{
constexpr int64_t SHADER_CACHE_LIMIT = 16 << 20;
/* The gamut-mapping 3DLUT alone is about 3 MB. */
constexpr int64_t SHADER_CACHE_OBJECT_LIMIT = 8 << 20;
/* Large enough for the biggest 3DLUT Kodi allows (256^3, 16-bit). */
constexpr int64_t ICC_CACHE_LIMIT = 128 << 20;

bool IsSemiPlanarFormat(AVPixelFormat format)
{
  return format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010 ||
         format == AV_PIX_FMT_P012 || format == AV_PIX_FMT_P016;
}

/* libplacebo never removes stale cache files, so enforce the size limit here by
 * deleting the least recently written entries. */
void PruneCacheDirectory(const std::string& dir, int64_t maxSize)
{
  CFileItemList items;
  if (!XFILE::CDirectory::GetDirectory(dir, items, "", XFILE::DIR_FLAG_NO_FILE_DIRS))
    return;

  struct CacheFile
  {
    std::string path;
    int64_t size;
    int64_t mtime;
  };

  std::vector<CacheFile> files;
  int64_t totalSize = 0;
  for (const auto& item : items)
  {
    if (item->IsFolder())
      continue;

    files.push_back({item->GetPath(), item->GetSize(),
                     item->GetProperty(XFILE::DIR_PROPERTY_STAT_MTIME).asInteger(0)});
    totalSize += item->GetSize();
  }

  if (totalSize <= maxSize)
    return;

  std::sort(files.begin(), files.end(),
            [](const CacheFile& a, const CacheFile& b) { return a.mtime < b.mtime; });

  for (const auto& file : files)
  {
    if (totalSize <= maxSize)
      break;

    if (XFILE::CFile::Delete(file.path))
      totalSize -= file.size;
  }
}

/* libplacebo leaves the GL context in its own state (unbound VAO, framebuffer 0,
 * disabled scissor/blend, custom viewport), which breaks Kodi's subsequent GUI
 * and OSD drawing. Save and restore everything Kodi relies on. */
class CGLStateGuard
{
public:
  CGLStateGuard()
  {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFbo);
    glGetIntegerv(GL_VIEWPORT, m_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEquationAlpha);
    m_blend = glIsEnabled(GL_BLEND);
    m_scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    m_depthTest = glIsEnabled(GL_DEPTH_TEST);
    m_cullFace = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture2D);
  }

  ~CGLStateGuard() { Restore(); }

  CGLStateGuard(const CGLStateGuard&) = delete;
  CGLStateGuard& operator=(const CGLStateGuard&) = delete;

  void Restore()
  {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFbo);
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
    if (m_scissorTest)
      glEnable(GL_SCISSOR_TEST);
    else
      glDisable(GL_SCISSOR_TEST);
    if (m_depthTest)
      glEnable(GL_DEPTH_TEST);
    else
      glDisable(GL_DEPTH_TEST);
    if (m_cullFace)
      glEnable(GL_CULL_FACE);
    else
      glDisable(GL_CULL_FACE);
    glBlendFuncSeparate(m_blendSrcRgb, m_blendDstRgb, m_blendSrcAlpha, m_blendDstAlpha);
    glBlendEquationSeparate(m_blendEquationRgb, m_blendEquationAlpha);
    if (m_blend)
      glEnable(GL_BLEND);
    else
      glDisable(GL_BLEND);
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glActiveTexture(m_activeTexture);
    glBindTexture(GL_TEXTURE_2D, m_texture2D);
    glBindBuffer(GL_ARRAY_BUFFER, m_arrayBuffer);
  }

private:
  GLint m_drawFbo = 0;
  GLint m_readFbo = 0;
  GLint m_viewport[4] = {};
  GLint m_scissorBox[4] = {};
  GLint m_program = 0;
  GLint m_vao = 0;
  GLint m_activeTexture = GL_TEXTURE0;
  GLint m_arrayBuffer = 0;
  GLint m_blendSrcRgb = GL_ONE;
  GLint m_blendDstRgb = GL_ZERO;
  GLint m_blendSrcAlpha = GL_ONE;
  GLint m_blendDstAlpha = GL_ZERO;
  GLint m_blendEquationRgb = GL_FUNC_ADD;
  GLint m_blendEquationAlpha = GL_FUNC_ADD;
  GLint m_texture2D = 0;
  GLboolean m_blend = GL_FALSE;
  GLboolean m_scissorTest = GL_FALSE;
  GLboolean m_depthTest = GL_FALSE;
  GLboolean m_cullFace = GL_FALSE;
};

std::vector<uint8_t> ReadFile(const std::string& path)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return {};

  const std::streamsize size = file.tellg();
  if (size <= 0)
    return {};

  std::vector<uint8_t> data(static_cast<size_t>(size));

  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(data.data()), size))
    return {};

  return data;
}
} // namespace

CPlaceboRendererGL::CPictureBuffer::CPictureBuffer()
{
  memset(&fields, 0, sizeof(fields));
  memset(&image , 0, sizeof(image));
  memset(&pbo   , 0, sizeof(pbo));
  videoBuffer = nullptr;
  loaded = false;
}

CBaseRenderer* CPlaceboRendererGL::Create(CVideoBuffer *buffer)
{
  return new CPlaceboRendererGL();
}

bool CPlaceboRendererGL::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("default", CPlaceboRendererGL::Create);
  return true;
}

CPlaceboRendererGL::CPlaceboRendererGL()
{
  m_iFlags = 0;
  m_format = AV_PIX_FMT_NONE;
  m_pboUsed = false;

  std::tie(m_useDithering, m_ditherDepth) = CServiceBroker::GetWinSystem()->GetDitherSettings();
  // TODO: GL does not support Auto dithering yet; treat as 8-bit (previous default)
  if (m_ditherDepth == 0)
    m_ditherDepth = 8;

  m_fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();

  m_renderSystem = dynamic_cast<CRenderSystemGL*>(CServiceBroker::GetRenderSystem());
}

CPlaceboRendererGL::~CPlaceboRendererGL()
{
  UnInit();
}

bool CPlaceboRendererGL::ValidateRenderer()
{
  if (!m_bConfigured)
    return false;

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
    return false;

  int index = m_iYUVRenderBuffer;
  const CPictureBuffer& buf = m_buffers[index];

  if (!buf.fields[0][0].id)
    return false;

  return true;
}

bool CPlaceboRendererGL::ValidateRenderTarget()
{
  if (!m_bValidated)
  {
    // function pointer for texture might change in
    // call to LoadShaders
    glFinish();
    for (int i = 0 ; i < NUM_BUFFERS ; i++)
    {
      DeleteTexture(i);
    }

    if (!InitPlacebo())
      return false;

    // trigger update of video filters
    m_scalingMethodGui = VS_SCALINGMETHOD_MAX;

    if (m_textureTarget == GL_TEXTURE_RECTANGLE)
      CLog::Log(LOGINFO, "Using GL_TEXTURE_RECTANGLE");
    else
      CLog::Log(LOGINFO, "Using GL_TEXTURE_2D");

    for (int i = 0; i < m_NumYUVBuffers; i++)
      CreateTexture(i);

    m_bValidated = true;
    return true;
  }
  return false;
}

static void logCallback(void* priv, enum pl_log_level logLevel, const char* logMessage)
{
  switch (logLevel) 
  {
  case PL_LOG_WARN:
    CLog::Log(LOGWARNING, "[Libplacebo] {}", logMessage);
    break;
  case PL_LOG_DEBUG:
  case PL_LOG_TRACE:
    CLog::Log(LOGDEBUG, "[Libplacebo] {}", logMessage);
    break;
  case PL_LOG_ERR:
    CLog::Log(LOGERROR, "[Libplacebo] {}", logMessage);
    break;
  case PL_LOG_INFO:
    CLog::Log(LOGINFO, "[Libplacebo] {}", logMessage);
    break;
  case PL_LOG_FATAL:
    CLog::Log(LOGFATAL, "[Libplacebo] {}", logMessage);
    break;
  default:
    CLog::Log(LOGINFO, "[Libplacebo] {}", logMessage);
    break;
  }
}

void CPlaceboRendererGL::InitIcc()
{
  if (m_iccProfile)
  {
    return;
  }

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings->GetBool("videoscreen.cmsenabled"))
  {
    return;
  }

  if (settings->GetInt("videoscreen.cmsmode") != CMS_MODE_PROFILE)
  {
    return;
  }

  m_iccPath = settings->GetString("videoscreen.displayprofile");
  if (m_iccPath.empty())
  {
    return;
  }

  const std::vector<uint8_t> iccData = ReadFile(m_iccPath);
  if (iccData.empty())
  {
    CLog::Log(LOGWARNING, "Failed to read ICC profile: {}", m_iccPath);
    return;
  }
  pl_icc_profile p = 
  {
    .data = iccData.data(),
    .len = iccData.size(),
    .signature = 0
  };
  pl_icc_profile_compute_signature(&p);

  m_iccLutSize = 1 << settings->GetInt("videoscreen.cmslutsize");
  m_iccParams.size_r = m_iccParams.size_g = m_iccParams.size_b = m_iccLutSize;
  m_iccParams.use_vcgt = true;
  m_iccParams.vcgt_size = 256;
  m_iccParams.intent = PL_INTENT_RELATIVE_COLORIMETRIC;
  if (m_iccCache)
  {
    m_iccParams.cache = m_iccCache;
  }

  if (!pl_icc_update(m_log, &m_iccProfile, &p, &m_iccParams))
  {
    CLog::Log(LOGWARNING, "Failed to open ICC Profile");
    m_iccPath.clear();
    m_iccLutSize = 0;
    return;
  }
}

void CPlaceboRendererGL::CheckIccParams()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  std::string path = settings->GetString("videoscreen.displayprofile");
  int lutSize = 1 << settings->GetInt("videoscreen.cmslutsize");

  if (settings->GetInt("videoscreen.cmsmode") != CMS_MODE_PROFILE || 
      !settings->GetBool("videoscreen.cmsenabled") ||
      path.empty())
  {
    if (m_iccProfile)
    {
      pl_icc_close(&m_iccProfile);
      m_iccProfile = nullptr;
    }
    m_iccPath.clear();
    m_iccLutSize = 0;
    m_renderDirty = true;
    return;
  }

  if (path == m_iccPath && lutSize == m_iccLutSize)
  {
    return;
  }

  const std::vector<uint8_t> iccData = ReadFile(path);
  if (iccData.empty())
  {
    CLog::Log(LOGWARNING, "Failed to read ICC profile: {}", path);
    if (m_iccProfile)
    {
      pl_icc_close(&m_iccProfile);
      m_iccProfile = nullptr;
    }
    m_iccPath.clear();
    m_iccLutSize = 0;
    m_renderDirty = true;
    return;
  }
  pl_icc_profile p = 
  {
    .data = iccData.data(),
    .len = iccData.size(),
    .signature = 0
  };
  pl_icc_profile_compute_signature(&p);

  m_iccPath = path;
  m_iccLutSize = lutSize;
  m_iccParams.size_r = m_iccParams.size_g = m_iccParams.size_b = m_iccLutSize;
  m_iccParams.intent = PL_INTENT_RELATIVE_COLORIMETRIC;
  m_iccParams.use_vcgt = true;
  m_iccParams.vcgt_size = 256;
  if (m_iccCache)
  {
    m_iccParams.cache = m_iccCache;
  }

  if (!pl_icc_update(m_log, &m_iccProfile, &p, &m_iccParams))
  {
    CLog::Log(LOGWARNING, "Failed to update icc profile");
    m_iccPath.clear();
    m_iccLutSize = 0;
    return;
  }

  m_renderDirty = true;
}

void CPlaceboRendererGL::InitCache()
{
  const std::string base = URIUtils::AddFileToFolder(
      CSpecialProtocol::TranslatePath("special://temp/"), "placebo");
  m_shaderCacheDir = URIUtils::AddFileToFolder(base, "shader");
  m_iccCacheDir = URIUtils::AddFileToFolder(base, "icc");

  if (!XFILE::CDirectory::Create(m_shaderCacheDir) ||
      !XFILE::CDirectory::Create(m_iccCacheDir))
  {
    CLog::Log(LOGWARNING, "PlaceboRendererGL: Failed to create cache directory {}", base);
    m_shaderCacheDir.clear();
    m_iccCacheDir.clear();
    return;
  }

  URIUtils::AddSlashAtEnd(m_shaderCacheDir);
  URIUtils::AddSlashAtEnd(m_iccCacheDir);

  PruneCacheDirectory(m_shaderCacheDir, SHADER_CACHE_LIMIT);
  PruneCacheDirectory(m_iccCacheDir, ICC_CACHE_LIMIT);

  struct pl_cache_params shaderParams = {};
  shaderParams.log = m_log;
  shaderParams.max_object_size = SHADER_CACHE_OBJECT_LIMIT;
  shaderParams.max_total_size = SHADER_CACHE_LIMIT;
  shaderParams.set = pl_cache_set_file;
  shaderParams.get = pl_cache_get_file;
  shaderParams.priv = m_shaderCacheDir.data();
  m_shaderCache = pl_cache_create(&shaderParams);

  struct pl_cache_params iccParams = {};
  iccParams.log = m_log;
  iccParams.max_object_size = ICC_CACHE_LIMIT;
  iccParams.max_total_size = ICC_CACHE_LIMIT;
  iccParams.set = pl_cache_set_file;
  iccParams.get = pl_cache_get_file;
  iccParams.priv = m_iccCacheDir.data();
  m_iccCache = pl_cache_create(&iccParams);

  pl_gpu_set_cache(m_gpu, m_shaderCache);

  CLog::Log(LOGDEBUG, "PlaceboRendererGL: Using shader and LUT cache in {}", base);
}

bool CPlaceboRendererGL::InitPlacebo()
{
  if (m_opengl)
    return true;

  struct pl_log_params logParams = 
  {
    .log_cb = logCallback,
    .log_priv = nullptr,
    .log_level = PL_LOG_ALL
  };
  m_log = pl_log_create(PL_API_VER, &logParams);
  if (!m_log) 
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL::InitPlacebo: Log initialisation failed");
    return false;
  }

  struct pl_opengl_params glParams = pl_opengl_default_params;
  m_opengl = pl_opengl_create(m_log, &glParams);
  if (!m_opengl) 
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL::InitPlacebo: OpenGL device creation failed");
    return false;
  }
  m_gpu = m_opengl->gpu;

  InitCache();

  m_renderer = pl_renderer_create(m_log, m_gpu);
  if (!m_renderer) 
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL::InitPlacebo: Renderer initialisation failed");
    return false;
  }

  InitIcc();

  return true;
}

AVPixelFormat CPlaceboRendererGL::GetSourceFormat(const VideoPicture& picture) const
{
  return picture.videoBuffer ? picture.videoBuffer->GetFormat() : AV_PIX_FMT_NONE;
}

bool CPlaceboRendererGL::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  m_format = GetSourceFormat(picture);
  if (m_format == AV_PIX_FMT_NONE)
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL::Configure: unsupported pixel format");
    return false;
  }

  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;
  m_fps = fps;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsStereoMode(picture.stereoMode);

  m_srcPrimaries = picture.color_primaries;
  m_toneMap = false;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  m_bConfigured = true;
  m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  m_scalingMethod = m_videoSettings.m_ScalingMethod;

  // Ensure that textures are recreated and rendering starts only after the 1st
  // frame is loaded after every call to Configure().
  m_bValidated = false;

  m_pboSupported = CGLExtensions::IsExtensionSupported(CGLExtensions::ARB_pixel_buffer_object);

  if (!CServiceBroker::GetWinSystem()->SetVideoOutput(&picture))
    CLog::Log(LOGWARNING, "PlaceboRendererGL::Configure: SetVideoOutput failed");

  CServiceBroker::GetWinSystem()->SetColorimetry(&picture);

  m_passthroughHDR = CServiceBroker::GetWinSystem()->SetHDR(&picture);
  CLog::Log(LOGDEBUG, "PlaceboRendererGL::Configure: HDR passthrough: {}",
            m_passthroughHDR ? "on" : "off");

  m_hdrFboActive =
      m_passthroughHDR && CServiceBroker::GetWinSystem()->SetGuiCompositing(picture.color_transfer);
  if (m_passthroughHDR && !m_hdrFboActive)
    CLog::Log(LOGWARNING, "PlaceboRendererGL::Configure: HDR passthrough active but GUI "
                          "compositing not supported by windowing system");

  return true;
}

bool CPlaceboRendererGL::ConfigChanged(const VideoPicture &picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

void CPlaceboRendererGL::AddVideoPicture(const VideoPicture &picture, int index)
{
  CPictureBuffer &buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CLog::LogF(LOGERROR, "unreleased video buffer");
    buf.videoBuffer->Release();
  }
  buf.videoBuffer = picture.videoBuffer;
  buf.videoBuffer->Acquire();
  buf.loaded = false;
  buf.m_srcPrimaries = picture.color_primaries;
  buf.m_srcColSpace = picture.color_space;
  buf.m_srcColTransfer = picture.color_transfer;
  buf.m_srcFullRange = picture.color_range == 1;
  buf.m_srcBits = picture.colorBits;

  buf.hasDisplayMetadata = picture.hasDisplayMetadata;
  buf.displayMetadata = picture.displayMetadata;
  buf.lightMetadata = picture.lightMetadata;
  if (picture.hasLightMetadata && picture.lightMetadata.MaxCLL)
    buf.hasLightMetadata = picture.hasLightMetadata;
  buf.hasDynamicMetadata = picture.hasDynamicMetadata;
  buf.dynamicMetadata = picture.dynamicMetadata;
  buf.doviMetadata = picture.doviMetadata;
}

void CPlaceboRendererGL::ReleaseBuffer(int idx)
{
  CPictureBuffer &buf = m_buffers[idx];
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

void CPlaceboRendererGL::GetPlaneTextureSize(CYuvPlane& plane)
{
  /* texture is assumed to be bound */
  GLint width  = 0
      , height = 0
      , border = 0;
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_WIDTH , &width);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_HEIGHT, &height);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_BORDER, &border);
  plane.texwidth  = width  - 2 * border;
  plane.texheight = height - 2 * border;
  if(plane.texwidth <= 0 || plane.texheight <= 0)
  {
    CLog::Log(LOGDEBUG, "CLinuxRendererGL::GetPlaneTextureSize - invalid size {}x{} - {}", width,
              height, border);
    /* to something that avoid division by zero */
    plane.texwidth  = 1;
    plane.texheight = 1;
  }
}

void CPlaceboRendererGL::CalculateTextureSourceRects(int source, int num_planes)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im  = &buf.image;

  // calculate the source rectangle
  for(int field = 0; field < 3; field++)
  {
    for(int plane = 0; plane < num_planes; plane++)
    {
      CYuvPlane& p = buf.fields[field][plane];

      // Copy texture dimensions and internal format from FIELD_FULL to all fields (they share the same texture)
      if (field != FIELD_FULL)
      {
        p.texwidth = buf.fields[FIELD_FULL][plane].texwidth;
        p.texheight = buf.fields[FIELD_FULL][plane].texheight;
        p.internalFormat = buf.fields[FIELD_FULL][plane].internalFormat;
      }

      p.rect = m_sourceRect;
      p.width  = im->width;
      p.height = im->height;

      if(field != FIELD_FULL)
      {
        /* correct for field offsets and chroma offsets */
        float offset_y = 0.5f;
        if(plane != 0)
          offset_y += 0.5f;
        if(field == FIELD_BOT)
          offset_y *= -1;

        p.rect.y1 += offset_y;
        p.rect.y2 += offset_y;

        /* half the height if this is a field */
        p.height  *= 0.5f;
        p.rect.y1 *= 0.5f;
        p.rect.y2 *= 0.5f;
      }

      if(plane != 0)
      {
        p.width   /= 1 << im->cshift_x;
        p.height  /= 1 << im->cshift_y;

        p.rect.x1 /= 1 << im->cshift_x;
        p.rect.x2 /= 1 << im->cshift_x;
        p.rect.y1 /= 1 << im->cshift_y;
        p.rect.y2 /= 1 << im->cshift_y;
      }

      // protect against division by zero
      if (p.texheight == 0 || p.texwidth == 0 ||
          p.pixpertex_x == 0 || p.pixpertex_y == 0)
      {
        continue;
      }

      p.height  /= p.pixpertex_y;
      p.rect.y1 /= p.pixpertex_y;
      p.rect.y2 /= p.pixpertex_y;
      p.width   /= p.pixpertex_x;
      p.rect.x1 /= p.pixpertex_x;
      p.rect.x2 /= p.pixpertex_x;

      if (m_textureTarget == GL_TEXTURE_2D)
      {
        p.height  /= p.texheight;
        p.rect.y1 /= p.texheight;
        p.rect.y2 /= p.texheight;
        p.width   /= p.texwidth;
        p.rect.x1 /= p.texwidth;
        p.rect.x2 /= p.texwidth;
      }
    }
  }
}

void CPlaceboRendererGL::LoadPlane(CYuvPlane& plane, int type,
                                 unsigned width, unsigned height,
                                 int stride, int bpp, void* data)
{

  if (plane.pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, plane.pbo);

  int bps = bpp * KODI::UTILS::GL::glFormatElementByteCount(type);

  unsigned datatype;
  if (bpp == 2)
    datatype = GL_UNSIGNED_SHORT;
  else
    datatype = GL_UNSIGNED_BYTE;

  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / bps);
  glBindTexture(m_textureTarget, plane.id);
  glTexSubImage2D(m_textureTarget, 0, 0, 0, width, height, type, datatype, data);

  /* check if we need to load any border pixels */
  if (height < plane.texheight)
    glTexSubImage2D( m_textureTarget, 0
                   , 0, height, width, 1
                   , type, datatype
                   , (unsigned char*)data + stride * (height-1));

  if (width  < plane.texwidth)
    glTexSubImage2D( m_textureTarget, 0
                   , width, 0, 1, height
                   , type, datatype
                   , (unsigned char*)data + bps * (width-1));

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(m_textureTarget, 0);
  if (plane.pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

bool CPlaceboRendererGL::Flush(bool saveBuffers)
{
  bool safe = saveBuffers && CanSaveBuffers();
  glFinish();

  for (int i = 0; i < m_NumYUVBuffers; i++)
  {
    if (!safe)
      ReleaseBuffer(i);
    DeleteTexture(i);
  }

  glFinish();
  m_bValidated = false;
  m_iYUVRenderBuffer = 0;

  return safe;
}

void CPlaceboRendererGL::Update()
{
  if (!m_bConfigured)
    return;

  ManageRenderArea();
  ValidateRenderTarget();
}

void CPlaceboRendererGL::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  m_renderAlpha = alpha;

  if (index2 >= 0)
    m_iYUVRenderBuffer = index2;
  else
    m_iYUVRenderBuffer = index;

  if (!ValidateRenderer())
  {
    if (clear) //if clear is set, we're expected to overwrite all backbuffer pixels, even if we have nothing to render
      ClearBackBuffer();

    return;
  }

  ManageRenderArea();

  if (clear)
  {
    //draw black bars when video is not transparent, clear the entire backbuffer when it is
    if (alpha == 255)
      DrawBlackBars();
    else
      ClearBackBuffer();
  }

  if (alpha < 255)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  else
  {
    glDisable(GL_BLEND);
  }

  if (!Render(flags, m_iYUVRenderBuffer) && clear)
    ClearBackBuffer();

  if (index2 >= 0)
  {
    m_iYUVRenderBuffer = index;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Render(flags, m_iYUVRenderBuffer);
  }

  VerifyGLState();
  glEnable(GL_BLEND);
  glFlush();
}

void CPlaceboRendererGL::ClearBackBuffer()
{
  //set the entire backbuffer to black
  //if we do a two pass render, we have to draw a quad. else we might occlude OSD elements.
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetRenderOrder() ==
      RENDER_ORDER_ALL_BACK_TO_FRONT)
  {
    CServiceBroker::GetWinSystem()->GetGfxContext().Clear(0xff000000);
  }
  else
  {
    ClearBackBufferQuad();
  }
}

void CPlaceboRendererGL::ClearBackBufferQuad()
{
  CRect windowRect(0, 0, CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth(),
                   CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight());
  struct Svertex
  {
    float x, y;
  };

  std::vector<Svertex> vertices{
      {windowRect.x1, windowRect.y2 * 2},
      {windowRect.x1, windowRect.y1},
      {windowRect.x2 * 2, windowRect.y1},
  };

  glDisable(GL_BLEND);

  m_renderSystem->EnableShader(ShaderMethodGL::SM_DEFAULT);
  GLint posLoc = m_renderSystem->ShaderGetPos();
  GLint uniCol = m_renderSystem->ShaderGetUniCol();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);

  GLuint vertexVBO;
  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, vertices.size());

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  m_renderSystem->DisableShader();
}

//draw black bars around the video quad, this is more efficient than glClear()
//since it only sets pixels to black that aren't going to be overwritten by the video
void CPlaceboRendererGL::DrawBlackBars()
{
  glDisable(GL_BLEND);

  struct Svertex
  {
    float x,y,z;
  };
  Svertex vertices[24];
  GLubyte count = 0;

  m_renderSystem->EnableShader(ShaderMethodGL::SM_DEFAULT);
  GLint posLoc = m_renderSystem->ShaderGetPos();
  GLint uniCol = m_renderSystem->ShaderGetUniCol();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);

  int osWindowWidth = CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth();
  int osWindowHeight = CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight();

  //top quad
  if (m_destRect.y1 > 0.0f)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = 0.0;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = 0;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = m_destRect.y1;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_destRect.y1;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // bottom quad
  if (m_destRect.y2 < osWindowHeight)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_destRect.y2;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = m_destRect.y2;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = osWindowHeight;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = osWindowHeight;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // left quad
  if (m_destRect.x1 > 0.0f)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_destRect.y1;
    vertices[quad].z = 0;
    vertices[quad+1].x = m_destRect.x1;
    vertices[quad+1].y = m_destRect.y1;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = m_destRect.x1;
    vertices[quad+2].y = m_destRect.y2;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_destRect.y2;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // right quad
  if (m_destRect.x2 < osWindowWidth)
  {
    GLubyte quad = count;
    vertices[quad].x = m_destRect.x2;
    vertices[quad].y = m_destRect.y1;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = m_destRect.y1;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = m_destRect.y2;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = m_destRect.x2;
    vertices[quad+4].y = m_destRect.y2;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  GLuint vertexVBO;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex)*count, &vertices[0], GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, count);

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  m_renderSystem->DisableShader();
}

void CPlaceboRendererGL::UnInit()
{
  CLog::Log(LOGDEBUG, "LinuxRendererGL: Cleaning up GL resources");
  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  glFinish();

  // YUV textures
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    ReleaseBuffer(i);
    DeleteTexture(i);
  }

  if (m_targetTex)
    pl_tex_destroy(m_gpu, &m_targetTex);
  m_targetFbo.Cleanup();
  m_targetWidth = 0;
  m_targetHeight = 0;
  m_renderDirty = true;
  m_lastRenderedBuffer = -1;
  m_lastRenderedField = -1;

  if (m_renderer)
    pl_renderer_destroy(&m_renderer);
  if (m_opengl)
    pl_opengl_destroy(&m_opengl);
  if (m_log)
    pl_log_destroy(&m_log);
  if (m_iccProfile)
    pl_icc_close(&m_iccProfile);

  if (m_shaderCache)
    CLog::Log(LOGDEBUG, "PlaceboRendererGL: Shader cache holds {} objects",
              pl_cache_objects(m_shaderCache));
  pl_cache_destroy(&m_iccCache);
  pl_cache_destroy(&m_shaderCache);
  m_iccParams.cache = nullptr;
  m_shaderCacheDir.clear();
  m_iccCacheDir.clear();

  m_gpu = nullptr;

  if (m_bConfigured)
  {
    m_hdrFboActive = false;
    CServiceBroker::GetWinSystem()->SetGuiCompositing(false);
    CServiceBroker::GetWinSystem()->SetHDR(nullptr);
    m_passthroughHDR = false;
    CServiceBroker::GetWinSystem()->SetColorimetry(nullptr);
    CServiceBroker::GetWinSystem()->SetVideoOutput(nullptr);
  }

  // cleanup framebuffer object if it was in use
  m_bValidated = false;
  m_bConfigured = false;
}

bool CPlaceboRendererGL::IsGuiLayer()
{
  return !m_hdrFboActive;
}

bool CPlaceboRendererGL::Render(unsigned int flags, int renderBuffer)
{
  // obtain current field, if interlaced
  if( flags & RENDER_FLAG_TOP)
    m_currentField = FIELD_TOP;

  else if (flags & RENDER_FLAG_BOT)
    m_currentField = FIELD_BOT;

  else
    m_currentField = FIELD_FULL;

  // call texture load function
  if (!UploadTexture(renderBuffer))
  {
    return false;
  }

  RenderPlacebo(renderBuffer, m_currentField);
  AfterRenderHook(renderBuffer);
  return true;
}

bool CPlaceboRendererGL::UploadTexture(int index)
{
  if (!m_buffers[index].videoBuffer)
    return false;

  bool ret = true;

  if (!m_buffers[index].loaded)
  {
    YuvImage &dst = m_buffers[index].image;
    YuvImage src;
    m_buffers[index].videoBuffer->GetPlanes(src.plane);
    m_buffers[index].videoBuffer->GetStrides(src.stride);

    UnBindPbo(m_buffers[index]);

    if (m_format == AV_PIX_FMT_NV12)
    {
      CVideoBuffer::CopyNV12Picture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadNV12Texture(index);
    }
    else if (m_format == AV_PIX_FMT_YUYV422 ||
             m_format == AV_PIX_FMT_UYVY422)
    {
      CVideoBuffer::CopyYUV422PackedPicture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadPackedYUVTexture(index);
    }
    else
    {
      CVideoBuffer::CopyPicture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadPlanarYUVTexture(index);
    }

    if (ret)
    {
      m_buffers[index].loaded = true;
      m_renderDirty = true;
    }
  }

  if (ret)
    CalculateTextureSourceRects(index, 3);

  return ret;
}

void CPlaceboRendererGL::DeleteTexture(int index)
{
  m_buffers[index].loaded = false;

  if (m_format == AV_PIX_FMT_NV12)
    DeleteNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
    DeletePackedYUVTexture(index);
  else
    DeletePlanarYUVTexture(index);
}

bool CPlaceboRendererGL::CreateTexture(int index)
{
  if (m_format == AV_PIX_FMT_NV12)
  {
    CLog::Log(LOGINFO, "Creating NV12 texture");
    return CreateNV12Texture(index);
  }
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
  {
    CLog::Log(LOGINFO, "Creating packed YUV texture");
    return CreatePackedYUVTexture(index);
  }
  else
  {
    CLog::Log(LOGINFO, "Creating Planar YUV");
    return CreatePlanarYUVTexture(index);
  }
}

GLint CPlaceboRendererGL::GetInternalFormat(GLint format, int bpp)
{
  unsigned int major, minor;
  m_renderSystem->GetRenderVersion(major, minor);
  if (bpp == 2)
  {
    if (format == GL_RED)
    {
      if (major > 2)
        return GL_R16;
      else
        return GL_LUMINANCE16;
    }
  }
  else
  {
    if (format == GL_RED)
    {
      if (major > 2)
        return GL_R8;
      else
        return GL_LUMINANCE;
    }
  }

  return format;
}

//********************************************************************************************************
// YUV texture creation, deletion, copying + clearing
//********************************************************************************************************

bool CPlaceboRendererGL::CreatePlanarYUVTexture(int index)
{
  /* since we also want the field textures, pitch must be texture aligned */
  unsigned p;

  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DeletePlanarYUVTexture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  // Chroma subsampling and bit depth derived from libavutil's pixdesc API,
  // avoiding a parallel per-pix_fmt switch. Works for any planar YUV pix_fmt
  // (4:2:0 / 4:2:2 / 4:4:4 at any supported bit depth).
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(m_format);
  im.cshift_x = desc ? desc->log2_chroma_w : 1;
  im.cshift_y = desc ? desc->log2_chroma_h : 1;
  buf.m_srcTextureBits = desc ? desc->comp[0].depth : 8;
  im.bpp = (buf.m_srcTextureBits > 8) ? 2 : 1;

  im.stride[0] = im.bpp * im.width;
  im.stride[1] = im.bpp * (im.width >> im.cshift_x);
  im.stride[2] = im.bpp * (im.width >> im.cshift_x);

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = im.stride[1] * (im.height >> im.cshift_y);
  im.planesize[2] = im.stride[2] * (im.height >> im.cshift_y);

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(3, pbo);

    for (int i = 0; i < 3; i++)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW);
      void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*) pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 3; i++)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      }
      glDeleteBuffers(3, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 3; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(p = 0;p<YuvImage::MAX_PLANES;p++)
    {
      if (!glIsTexture(m_buffers[index].fields[f][p].id))
      {
        glGenTextures(1, &m_buffers[index].fields[f][p].id);
        VerifyGLState();
      }
      m_buffers[index].fields[f][p].pbo = pbo[p];
    }
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
    CYuvPlane (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[f];

    planes[0].texwidth  = im.width;
    planes[0].texheight = im.height >> fieldshift;

    planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[1].texheight = planes[0].texheight >> im.cshift_y;
    planes[2].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[2].texheight = planes[0].texheight >> im.cshift_y;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 1;
      planes[p].pixpertex_y = 1;
    }

    for (int p = 0; p < 3; p++)
    {
      CYuvPlane &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
        continue;

      glBindTexture(m_textureTarget, plane.id);
      GLint internalformat;
      internalformat = GetInternalFormat(GL_RED, im.bpp);
      plane.internalFormat = internalformat;
      if (im.bpp == 2)
        glTexImage2D(m_textureTarget, 0, internalformat, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_SHORT, NULL);
      else
        glTexImage2D(m_textureTarget, 0, internalformat, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }
  return true;
}

void CPlaceboRendererGL::DestroyWrappedTextures(CPictureBuffer& buf)
{
  for (int f = 0; f < MAX_FIELDS; f++)
  {
    for (int i = 0; i < YuvImage::MAX_PLANES; i++)
    {
      if (buf.fields[f][i].wrapped && m_gpu)
        pl_tex_destroy(m_gpu, &buf.fields[f][i].wrapped);
    }
  }
}

void CPlaceboRendererGL::DeletePlanarYUVTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DestroyWrappedTextures(buf);

  for (int i = 0; i < 3; i++)
  {
    if (buf.fields[FIELD_FULL][i].id)
    {
      glDeleteTextures(1, &buf.fields[FIELD_FULL][i].id);
      buf.fields[FIELD_FULL][i].id = 0;
    }
    if (pbo[i])
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glDeleteBuffers(1, &pbo[i]);
      pbo[i] = 0;
    }
    if (im.plane[i] && !m_pboUsed)
    {
      delete[] im.plane[i];
      im.plane[i] = nullptr;
    }
  }
}

bool CPlaceboRendererGL::UploadPlanarYUVTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;

  for (int i = 0; i < 3; i++)
  {
    if (buf.fields[FIELD_FULL][i].id)
    {
      if (buf.fields[FIELD_FULL][i].pbo)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf.fields[FIELD_FULL][i].pbo);

      GLenum format = GL_RED;
      GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

      int w = (i == 0) ? im.width : (im.width >> im.cshift_x);
      int h = (i == 0) ? im.height : (im.height >> im.cshift_y);

      glPixelStorei(GL_UNPACK_ROW_LENGTH, im.stride[i] / im.bpp);
      glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][i].id);
      glTexSubImage2D(m_textureTarget, 0, 0, 0, w, h, format, type, im.plane[i]);

      /* check if we need to load any border pixels */
      if (h < static_cast<int>(buf.fields[FIELD_FULL][i].texheight))
        glTexSubImage2D(m_textureTarget, 0, 0, h, w, 1, format, type,
                       (unsigned char*)im.plane[i] + im.stride[i] * (h-1));

      if (w < static_cast<int>(buf.fields[FIELD_FULL][i].texwidth))
        glTexSubImage2D(m_textureTarget, 0, w, 0, 1, h, format, type,
                       (unsigned char*)im.plane[i] + im.bpp * (w-1));

      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glBindTexture(m_textureTarget, 0);

      if (buf.fields[FIELD_FULL][i].pbo)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
  }

  return true;
}

bool CPlaceboRendererGL::CreateNV12Texture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DeleteNV12Texture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(m_format);
  im.cshift_x = desc ? desc->log2_chroma_w : 1;
  im.cshift_y = desc ? desc->log2_chroma_h : 1;
  buf.m_srcTextureBits = desc ? desc->comp[0].depth : 8;
  im.bpp = (buf.m_srcTextureBits > 8) ? 2 : 1;

  im.stride[0] = im.bpp * im.width;
  im.stride[1] = im.bpp * (im.width >> im.cshift_x);

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = im.stride[1] * (im.height >> im.cshift_y);

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(2, pbo);

    for (int i = 0; i < 2; i++)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW);
      void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*) pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 2; i++)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      }
      glDeleteBuffers(2, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 2; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  // Create GL textures for Y and UV planes
  // Y plane
  glGenTextures(1, &buf.fields[FIELD_FULL][0].id);
  glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][0].id);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum internalFormatY = (buf.m_srcTextureBits > 8) ? GL_R16 : GL_R8;
  GLenum formatY = GL_RED;
  GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

  glTexImage2D(m_textureTarget, 0, internalFormatY, im.width, im.height, 0, formatY, type, nullptr);

  // Store texture dimensions and internal format for later use
  buf.fields[FIELD_FULL][0].texwidth = im.width;
  buf.fields[FIELD_FULL][0].texheight = im.height;
  buf.fields[FIELD_FULL][0].internalFormat = internalFormatY;

  glBindTexture(m_textureTarget, 0);

  // UV plane (interleaved)
  glGenTextures(1, &buf.fields[FIELD_FULL][1].id);
  glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][1].id);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum internalFormatUV = (buf.m_srcTextureBits > 8) ? GL_RG16 : GL_RG8;
  GLenum formatUV = GL_RG;

  int uv_w = im.width >> im.cshift_x;
  int uv_h = im.height >> im.cshift_y;

  glTexImage2D(m_textureTarget, 0, internalFormatUV, uv_w, uv_h, 0, formatUV, type, nullptr);

  // Store texture dimensions and internal format for later use
  buf.fields[FIELD_FULL][1].texwidth = uv_w;
  buf.fields[FIELD_FULL][1].texheight = uv_h;
  buf.fields[FIELD_FULL][1].internalFormat = internalFormatUV;

  glBindTexture(m_textureTarget, 0);

  return true;
}

void CPlaceboRendererGL::DeleteNV12Texture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DestroyWrappedTextures(buf);

  for (int i = 0; i < 2; i++)
  {
    if (buf.fields[FIELD_FULL][i].id)
    {
      glDeleteTextures(1, &buf.fields[FIELD_FULL][i].id);
      buf.fields[FIELD_FULL][i].id = 0;
    }
    if (pbo[i])
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glDeleteBuffers(1, &pbo[i]);
      pbo[i] = 0;
    }
    if (im.plane[i] && !m_pboUsed)
    {
      delete[] im.plane[i];
      im.plane[i] = nullptr;
    }
  }
}

bool CPlaceboRendererGL::UploadNV12Texture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;

  // Y plane
  if (buf.fields[FIELD_FULL][0].id)
  {
    if (buf.fields[FIELD_FULL][0].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf.fields[FIELD_FULL][0].pbo);

    GLenum format = GL_RED;
    GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, im.stride[0] / (im.bpp * (type == GL_UNSIGNED_SHORT ? 2 : 1)));
    glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][0].id);
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width, im.height, format, type, im.plane[0]);

    if (im.height < buf.fields[FIELD_FULL][0].texheight)
      glTexSubImage2D(m_textureTarget, 0, 0, im.height, im.width, 1, format, type,
                     (unsigned char*)im.plane[0] + im.stride[0] * (im.height-1));

    if (im.width < buf.fields[FIELD_FULL][0].texwidth)
      glTexSubImage2D(m_textureTarget, 0, im.width, 0, 1, im.height, format, type,
                     (unsigned char*)im.plane[0] + im.bpp * (im.width-1));

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(m_textureTarget, 0);

    if (buf.fields[FIELD_FULL][0].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  // UV plane
  if (buf.fields[FIELD_FULL][1].id)
  {
    if (buf.fields[FIELD_FULL][1].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf.fields[FIELD_FULL][1].pbo);

    GLenum format = GL_RG;
    GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

    int uv_w = im.width >> im.cshift_x;
    int uv_h = im.height >> im.cshift_y;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, im.stride[1] / (im.bpp * (type == GL_UNSIGNED_SHORT ? 2 : 1) * 2));
    glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][1].id);
    glTexSubImage2D(m_textureTarget, 0, 0, 0, uv_w, uv_h, format, type, im.plane[1]);

    if (uv_h < static_cast<int>(buf.fields[FIELD_FULL][1].texheight))
      glTexSubImage2D(m_textureTarget, 0, 0, uv_h, uv_w, 1, format, type,
                     (unsigned char*)im.plane[1] + im.stride[1] * (uv_h-1));

    if (uv_w < static_cast<int>(buf.fields[FIELD_FULL][1].texwidth))
      glTexSubImage2D(m_textureTarget, 0, uv_w, 0, 1, uv_h, format, type,
                     (unsigned char*)im.plane[1] + im.bpp * 2 * (uv_w-1));

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(m_textureTarget, 0);

    if (buf.fields[FIELD_FULL][1].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  return true;
}

bool CPlaceboRendererGL::CreatePackedYUVTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DeletePackedYUVTexture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(m_format);
  im.cshift_x = desc ? desc->log2_chroma_w : 1;
  im.cshift_y = desc ? desc->log2_chroma_h : 1;
  buf.m_srcTextureBits = desc ? desc->comp[0].depth : 8;
  im.bpp = (buf.m_srcTextureBits > 8) ? 4 : 2; // packed YUV is 2 bytes per pixel (YUYV) or 4 for 10-bit

  im.stride[0] = im.bpp * im.width;
  im.planesize[0] = im.stride[0] * im.height;

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(1, pbo);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[0]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[0] + PBO_OFFSET, 0, GL_STREAM_DRAW);
    void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (pboPtr)
    {
      im.plane[0] = (uint8_t*) pboPtr + PBO_OFFSET;
      memset(im.plane[0], 0, im.planesize[0]);
    }
    else
    {
      CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
      pboSetup = false;
    }

    if (!pboSetup)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[0]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glDeleteBuffers(1, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    im.plane[0] = new uint8_t[im.planesize[0]];
  }

  // Create GL texture for packed YUV
  glGenTextures(1, &buf.fields[FIELD_FULL][0].id);
  glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][0].id);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum internalFormat = (buf.m_srcTextureBits > 8) ? GL_RGBA16 : GL_RGBA8;
  GLenum format = GL_RGBA;
  GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

  glTexImage2D(m_textureTarget, 0, internalFormat, im.width, im.height, 0, format, type, nullptr);

  // Store texture dimensions and internal format for later use
  buf.fields[FIELD_FULL][0].texwidth = im.width;
  buf.fields[FIELD_FULL][0].texheight = im.height;
  buf.fields[FIELD_FULL][0].internalFormat = internalFormat;

  glBindTexture(m_textureTarget, 0);

  return true;
}

void CPlaceboRendererGL::DeletePackedYUVTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DestroyWrappedTextures(buf);

  if (buf.fields[FIELD_FULL][0].id)
  {
    glDeleteTextures(1, &buf.fields[FIELD_FULL][0].id);
    buf.fields[FIELD_FULL][0].id = 0;
  }
  if (pbo[0])
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[0]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glDeleteBuffers(1, &pbo[0]);
    pbo[0] = 0;
  }
  if (im.plane[0] && !m_pboUsed)
  {
    delete[] im.plane[0];
    im.plane[0] = nullptr;
  }
}

bool CPlaceboRendererGL::UploadPackedYUVTexture(int index)
{
  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;

  if (buf.fields[FIELD_FULL][0].id)
  {
    if (buf.fields[FIELD_FULL][0].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf.fields[FIELD_FULL][0].pbo);

    GLenum format = GL_RGBA;
    GLenum type = (buf.m_srcTextureBits > 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, im.stride[0] / (im.bpp * (type == GL_UNSIGNED_SHORT ? 2 : 1)));
    glBindTexture(m_textureTarget, buf.fields[FIELD_FULL][0].id);
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width, im.height, format, type, im.plane[0]);

    if (im.height < buf.fields[FIELD_FULL][0].texheight)
      glTexSubImage2D(m_textureTarget, 0, 0, im.height, im.width, 1, format, type,
                     (unsigned char*)im.plane[0] + im.stride[0] * (im.height-1));

    if (im.width < buf.fields[FIELD_FULL][0].texwidth)
      glTexSubImage2D(m_textureTarget, 0, im.width, 0, 1, im.height, format, type,
                     (unsigned char*)im.plane[0] + im.bpp * (im.width-1));

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(m_textureTarget, 0);

    if (buf.fields[FIELD_FULL][0].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  return true;
}

void CPlaceboRendererGL::BindPbo(CPictureBuffer& buff)
{
  if (!m_pboUsed)
    return;

  for (int i = 0; i < 3; i++)
  {
    if (buff.fields[FIELD_FULL][i].pbo)
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buff.fields[FIELD_FULL][i].pbo);
  }
}

void CPlaceboRendererGL::UnBindPbo(CPictureBuffer& buff)
{
  if (!m_pboUsed)
    return;

  for (int i = 0; i < 3; i++)
  {
    if (buff.fields[FIELD_FULL][i].pbo)
    {
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
  }
}

//********************************************************************************************************
// Libplacebo rendering
//********************************************************************************************************

void CPlaceboRendererGL::RenderPlacebo(int index, int field)
{
  if (!m_renderer || !m_opengl || !m_gpu)
    return;

  const int width = static_cast<int>(std::lround(m_destRect.Width()));
  const int height = static_cast<int>(std::lround(m_destRect.Height()));
  if (width <= 0 || height <= 0)
    return;

  CPictureBuffer& buf = m_buffers[index];
  const int fieldIdx = (field == FIELD_FULL) ? FIELD_FULL : field;

  CheckVideoParameters(index);
  CheckIccParams();

  CGLStateGuard stateGuard;

  if (!EnsureRenderTarget())
    return;

  // The expensive libplacebo passes only need to run when the frame or the
  // geometry changes; repeated vsyncs just re-composite the cached result.
  const bool needsRender =
      m_renderDirty || m_lastRenderedBuffer != index || m_lastRenderedField != fieldIdx ||
      m_lastRenderDest != m_destRect;

  if (needsRender)
  {
    // Wrap GL textures as pl_tex
    pl_tex y_tex = nullptr, u_tex = nullptr, v_tex = nullptr;

    if (IsSemiPlanarFormat(m_format))
    {
      // NV12/P010: Y + interleaved UV
      y_tex = WrapGLTexture(buf.fields[fieldIdx][0]);
      u_tex = WrapGLTexture(buf.fields[fieldIdx][1]);
    }
    else if (m_format == AV_PIX_FMT_YUYV422 || m_format == AV_PIX_FMT_UYVY422)
    {
      // Packed YUV - treat as single texture with 2 components (Y + UV packed)
      y_tex = WrapGLTexture(buf.fields[fieldIdx][0]);
    }
    else
    {
      // Planar YUV: Y + U + V
      y_tex = WrapGLTexture(buf.fields[fieldIdx][0]);
      u_tex = WrapGLTexture(buf.fields[fieldIdx][1]);
      v_tex = WrapGLTexture(buf.fields[fieldIdx][2]);
    }

    if (!y_tex)
    {
      CLog::Log(LOGERROR, "PlaceboRendererGL: Failed to wrap Y plane texture");
      return;
    }

    // Set up source frame. The crop is relative to the luma plane and is
    // mapped directly onto the target, so it must describe the visible part of
    // the source instead of the whole texture.
    pl_frame frame = {};
    frame.crop = {m_sourceRect.x1, m_sourceRect.y1, m_sourceRect.x2, m_sourceRect.y2};

    if (IsSemiPlanarFormat(m_format))
    {
      frame.num_planes = 2;
      frame.planes[0].texture = y_tex;
      frame.planes[0].components = 1;
      frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
      frame.planes[1].texture = u_tex;
      frame.planes[1].components = 2;
      frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
      frame.planes[1].component_mapping[1] = PL_CHANNEL_CR;
    }
    else if (m_format == AV_PIX_FMT_YUYV422 || m_format == AV_PIX_FMT_UYVY422)
    {
      frame.num_planes = 1;
      frame.planes[0].texture = y_tex;
      frame.planes[0].components = 2; // Packed YUV
      frame.planes[0].component_mapping[0] = 0;
      frame.planes[0].component_mapping[1] = 1;
    }
    else
    {
      frame.num_planes = 3;
      frame.planes[0].texture = y_tex;
      frame.planes[0].components = 1;
      frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
      frame.planes[1].texture = u_tex;
      frame.planes[1].components = 1;
      frame.planes[1].component_mapping[0] = PL_CHANNEL_U;
      frame.planes[2].texture = v_tex;
      frame.planes[2].components = 1;
      frame.planes[2].component_mapping[0] = PL_CHANNEL_V;
    }

    // Kodi uploads the top row first, so the planes are not flipped.
    for (int i = 0; i < frame.num_planes; i++)
      frame.planes[i].flipped = false;

    // Set chroma location
    switch (CONF_FLAGS_CHROMA_MASK(m_iFlags))
    {
      case CONF_FLAGS_CHROMA_CENTER:
        pl_frame_set_chroma_location(&frame, PL_CHROMA_CENTER);
        break;
      case CONF_FLAGS_CHROMA_LEFT:
        pl_frame_set_chroma_location(&frame, PL_CHROMA_LEFT);
        break;
      case CONF_FLAGS_CHROMA_TOPLEFT:
        pl_frame_set_chroma_location(&frame, PL_CHROMA_TOP_LEFT);
        break;
    }

    // Convert color space
    pl_color_repr repr = {};
    repr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    repr.alpha = PL_ALPHA_NONE;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(m_format);
    if (desc && desc->comp[0].depth > 8)
    {
      repr.bits.sample_depth = 16;
      repr.bits.color_depth = desc->comp[0].depth;
      repr.bits.bit_shift = desc->comp[0].shift;
    }

    switch (buf.m_srcColSpace)
    {
      case AVCOL_SPC_RGB:
        repr.sys = PL_COLOR_SYSTEM_RGB;
        break;
      case AVCOL_SPC_BT709:
        repr.sys = PL_COLOR_SYSTEM_BT_709;
        break;
      case AVCOL_SPC_BT2020_NCL:
        repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
        break;
      case AVCOL_SPC_BT2020_CL:
        repr.sys = PL_COLOR_SYSTEM_BT_2020_C;
        break;
      case AVCOL_SPC_ICTCP:
        repr.sys = PL_COLOR_SYSTEM_BT_2100_PQ;
        break;
      case AVCOL_SPC_SMPTE240M:
        repr.sys = PL_COLOR_SYSTEM_SMPTE_240M;
        break;
      case AVCOL_SPC_YCGCO:
        repr.sys = PL_COLOR_SYSTEM_YCGCO;
        break;
      default:
        repr.sys = PL_COLOR_SYSTEM_BT_709;
        break;
    }

    pl_color_space csp = {};
    // Primaries
    csp.primaries = pl_primaries_from_av(buf.m_srcPrimaries);

    // Transfer characteristic
    csp.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

    // HDR Metadata
    pl_av_hdr_metadata avHdr = {};
    if (buf.hasDisplayMetadata)
    {
      avHdr.mdm = &buf.displayMetadata;
    }
    if (buf.hasLightMetadata)
    {
      avHdr.clm = &buf.lightMetadata;
    }
    if (buf.hasDynamicMetadata)
    {
      avHdr.dhp = &buf.dynamicMetadata;
    }
    pl_map_hdr_metadata(&csp.hdr, &avHdr);

    // Dovi Metadata
    pl_dovi_metadata dovi = {};
    if (!buf.doviMetadata.empty())
    {
      pl_map_avdovi_metadata(
          &csp, &repr, &dovi,
          reinterpret_cast<const AVDOVIMetadata*>(buf.doviMetadata.data()));
    }

    frame.repr = repr;
    frame.color = csp;
    frame.field = PL_FIELD_NONE;

    // Render into the offscreen target, which is composited onto the
    // backbuffer below.
    pl_frame target_frame = {};
    target_frame.num_planes = 1;
    target_frame.planes[0].texture = m_targetTex;
    target_frame.planes[0].flipped = true;
    target_frame.planes[0].components = 3;
    target_frame.planes[0].component_mapping[0] = PL_CHANNEL_R;
    target_frame.planes[0].component_mapping[1] = PL_CHANNEL_G;
    target_frame.planes[0].component_mapping[2] = PL_CHANNEL_B;

    target_frame.crop = {m_destRect.x1, m_destRect.y1, 
                         m_destRect.x2, m_destRect.y2};

    target_frame.icc = m_iccProfile;

    pl_color_repr target_repr = {};
    target_repr.sys = PL_COLOR_SYSTEM_RGB;
    target_repr.levels = PL_COLOR_LEVELS_FULL;
    target_repr.alpha = PL_ALPHA_NONE;

    pl_color_space target_csp = {};
    target_csp.primaries = PL_COLOR_PRIM_BT_709;
    target_csp.transfer = PL_COLOR_TRC_GAMMA24;
    target_frame.repr = target_repr;
    target_frame.color = target_csp;

    pl_render_params render_params = pl_render_default_params;
    render_params.border = PL_CLEAR_SKIP;

    // Dithering
    pl_dither_params ditherParams = {};
    if (m_useDithering)
    {
      ditherParams = {
        .method = PL_DITHER_BLUE_NOISE,
        .lut_size = 6,
        .temporal = false,
        .transfer = target_csp.transfer,
      };
      render_params.dither_params = &ditherParams;
    }
    else 
    {
      render_params.dither_params = NULL;
    }

    // Scaling
    render_params.plane_upscaler = &pl_filter_catmull_rom;
    render_params.plane_downscaler = &pl_filter_catmull_rom;
    render_params.disable_linear_scaling = true;
    switch (m_scalingMethod)
    {
      case VS_SCALINGMETHOD_LANCZOS3:
        render_params.upscaler = &pl_filter_lanczos;
        render_params.downscaler = &pl_filter_lanczos;
        break;
      case VS_SCALINGMETHOD_SPLINE36_FAST:
        render_params.upscaler = &pl_filter_spline36;
        render_params.downscaler = &pl_filter_spline36;
        break;
      case VS_SCALINGMETHOD_CUBIC_CATMULL:
        render_params.upscaler = &pl_filter_catmull_rom;
        render_params.downscaler = &pl_filter_catmull_rom;
        break;
      case VS_SCALINGMETHOD_CUBIC_MITCHELL:
        render_params.upscaler = &pl_filter_mitchell;
        render_params.downscaler = &pl_filter_mitchell;
        break;
      default:
        render_params.upscaler = &pl_filter_lanczos;
        render_params.downscaler = &pl_filter_lanczos;
        break;
    }

    // Tone mapping
    pl_color_map_params colorMapParams = pl_color_map_default_params;
    colorMapParams.metadata = PL_HDR_METADATA_ANY;
    colorMapParams.gamut_mapping = &pl_gamut_map_perceptual;

    switch (m_toneMapMethod)
    {
      case VS_TONEMAPMETHOD_REINHARD:
        colorMapParams.tone_mapping_function = &pl_tone_map_reinhard;
        break;
      case VS_TONEMAPMETHOD_ACES:
        colorMapParams.tone_mapping_function = &pl_tone_map_spline;
        break;
      case VS_TONEMAPMETHOD_HABLE:
        colorMapParams.tone_mapping_function = &pl_tone_map_hable;
        break;
      case VS_TONEMAPMETHOD_BT2390:
        colorMapParams.tone_mapping_function = &pl_tone_map_bt2390;
        break;
      case VS_TONEMAPMETHOD_ST2094_10:
        colorMapParams.tone_mapping_function = &pl_tone_map_st2094_10;
        break;
      case VS_TONEMAPMETHOD_ST2094_40:
        colorMapParams.tone_mapping_function = &pl_tone_map_st2094_40;
        break;
      case VS_TONEMAPMETHOD_OFF:
      default:
        colorMapParams.tone_mapping_function = &pl_tone_map_clip;
        break;
    }
    if (m_passthroughHDR)
    {
      target_csp = csp;
    }

    // Contrast Recovery
    colorMapParams.contrast_smoothness = 3.5f;
    colorMapParams.contrast_recovery = 0.4f;

    render_params.color_map_params = &colorMapParams;

    if (!pl_render_image(m_renderer, &frame, &target_frame, &render_params))
    {
      CLog::Log(LOGERROR, "PlaceboRendererGL: Failed to render frame");
      return;
    }

    m_renderDirty = false;
    m_lastRenderedBuffer = index;
    m_lastRenderedField = fieldIdx;
    m_lastRenderDest = m_destRect;
  }
}

bool CPlaceboRendererGL::EnsureRenderTarget()
{
  GLint currentFbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFbo);

  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);

  const int backbufferWidth = viewport[2];
  const int backbufferHeight = viewport[3];

  if (m_targetTex &&
      m_targetWidth == backbufferWidth &&
      m_targetHeight == backbufferHeight)
  {
    return true;
  }

  if (m_targetTex)
    pl_tex_destroy(m_gpu, &m_targetTex);

  pl_opengl_wrap_params params = {};
  params.framebuffer = currentFbo;
  params.width = backbufferWidth;
  params.height = backbufferHeight;

  m_targetTex = pl_opengl_wrap(m_gpu, &params);
  if (!m_targetTex)
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL: Failed to wrap backbuffer");
    return false;
  }

  m_targetWidth = backbufferWidth;
  m_targetHeight = backbufferHeight;

  m_renderDirty = true;
  return true;
}

pl_tex CPlaceboRendererGL::WrapGLTexture(CYuvPlane& plane)
{
  if (!plane.id)
    return nullptr;

  if (plane.wrapped)
    return plane.wrapped;

  pl_opengl_wrap_params params{};
  params.texture = plane.id;
  params.width = plane.texwidth;
  params.height = plane.texheight;
  params.iformat = plane.internalFormat;
  params.target = m_textureTarget;

  plane.wrapped = pl_opengl_wrap(m_gpu, &params);
  if (!plane.wrapped)
  {
    CLog::Log(LOGERROR, "PlaceboRendererGL: Failed to wrap texture {} (w={}, h={}, fmt={})",
              plane.id, plane.texwidth, plane.texheight, plane.internalFormat);
  }

  return plane.wrapped;
}

bool CPlaceboRendererGL::SupportsMultiPassRendering()
{
  return false; // Single-pass only
}

bool CPlaceboRendererGL::Supports(ERENDERFEATURE feature) const
{
  switch (feature)
  {
    case RENDERFEATURE_TONEMAP:
      return true;
    default:
      return false;
  }
}

bool CPlaceboRendererGL::Supports(ESCALINGMETHOD method) const
{
  return method == VS_SCALINGMETHOD_CUBIC_CATMULL ||
         method == VS_SCALINGMETHOD_CUBIC_MITCHELL ||
         method == VS_SCALINGMETHOD_LANCZOS3 ||
         method == VS_SCALINGMETHOD_SPLINE36;
}

CRenderInfo CPlaceboRendererGL::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = NUM_BUFFERS;
  return info;
}

void CPlaceboRendererGL::CheckVideoParameters(int index)
{
  const CPictureBuffer& buf = m_buffers[index];
  const ETONEMAPMETHOD& toneMapMethod = m_videoSettings.m_ToneMapMethod;
  const ESCALINGMETHOD& scalingMethod = m_videoSettings.m_ScalingMethod;
  bool settingsChanged = false;

  if (m_scalingMethod != scalingMethod)
  {
    m_scalingMethod = scalingMethod;
    settingsChanged = true;
  }

  if (buf.m_srcPrimaries != m_srcPrimaries)
  {
    m_srcPrimaries = buf.m_srcPrimaries;
    settingsChanged = true;
  }

  bool toneMap = false;
  const bool streamIsHDRPQ =
      (buf.m_srcColTransfer == AVCOL_TRC_SMPTE2084 && buf.m_srcPrimaries == AVCOL_PRI_BT2020);
  const bool streamIsHDR =
      buf.m_srcPrimaries == AVCOL_PRI_BT2020 && (buf.m_srcColTransfer == AVCOL_TRC_SMPTE2084 ||
                                                 buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67);


  m_streamIsHDR = streamIsHDR;

  if (!m_passthroughHDR && streamIsHDRPQ && toneMapMethod != VS_TONEMAPMETHOD_OFF)
  {
    toneMap = true;
  }

  if (toneMap != m_toneMap || toneMapMethod != m_toneMapMethod)
  {
    //! @todo HACK for #28468; clean up properly in Piers beta 2
    if (toneMap != m_toneMap)
    {
      VideoPicture tmp{};
      tmp.color_space = toneMap ? AVCOL_SPC_UNSPECIFIED : buf.m_srcColSpace;
      tmp.iWidth = m_sourceWidth;
      tmp.iHeight = m_sourceHeight;
      CServiceBroker::GetWinSystem()->SetColorimetry(&tmp);
    }

    m_toneMap = toneMap;
    m_toneMapMethod = toneMapMethod;
    settingsChanged = true;
  }

  if (settingsChanged)
  {
    m_renderDirty = true;
  }
}

AVColorPrimaries CPlaceboRendererGL::GetSrcPrimaries(AVColorPrimaries srcPrimaries, unsigned int width, unsigned int height)
{
  (void)width;
  (void)height;
  return srcPrimaries;
}