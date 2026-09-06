/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GLESShader.h"

#include <algorithm>
#include <atomic>

#include "ServiceBroker.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "rendering/MatrixGL.h"
#include "rendering/RenderSystem.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingType.h"

using namespace Shaders;

namespace
{
constexpr GLuint GUI_VERTEX_BINDING_POINT = 3;
constexpr GLuint GUI_FRAGMENT_BINDING_POINT = 4;

struct GuiVertexBlockData
{
  std::array<GLfloat, 16> proj{};
  std::array<GLfloat, 16> model{};
};

struct GuiFragmentBlockData
{
  std::array<GLfloat, 4> guiParams0{};
  std::array<GLfloat, 4> guiParams1{};
};

void EnsureGuiUniformBuffer(GLuint& buffer, GLsizeiptr size)
{
  if (buffer == 0)
  {
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
  }
  else
  {
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
  }
}
} // namespace

CGLESShader::CGLESShader(const char* shader, const std::string& prefix)
{
  m_proj = nullptr;
  m_model = nullptr;
  m_clipPossible = false;

  VertexShader()->LoadSource("gles_shader.vert");
  PixelShader()->LoadSource(shader, prefix);
}

CGLESShader::CGLESShader(const char* vshader, const char* fshader, const std::string& prefix)
{
  m_proj = nullptr;
  m_model  = nullptr;
  m_clipPossible = false;

  VertexShader()->LoadSource(vshader, prefix);
  PixelShader()->LoadSource(fshader, prefix);
}

void CGLESShader::OnCompiledAndLinked()
{
  // This is called after CompileAndLink()

  // Variables passed directly to the Fragment shader
  m_hTex0   = glGetUniformLocation(ProgramHandle(), "m_samp0");
  m_hTex1   = glGetUniformLocation(ProgramHandle(), "m_samp1");
  m_hUniCol = glGetUniformLocation(ProgramHandle(), "m_unicol");
  m_hField  = glGetUniformLocation(ProgramHandle(), "m_field");
  m_hStep   = glGetUniformLocation(ProgramHandle(), "m_step");
  m_hContrast   = glGetUniformLocation(ProgramHandle(), "m_contrast");
  m_hBrightness = glGetUniformLocation(ProgramHandle(), "m_brightness");
  m_sdrPeak = glGetUniformLocation(ProgramHandle(), "m_sdrPeak");
  m_sdrSaturation = glGetUniformLocation(ProgramHandle(), "m_sdrSaturation");
  m_hdrPgsPeak = glGetUniformLocation(ProgramHandle(), "m_hdrPgsPeak");
  m_hdrPgsSaturation = glGetUniformLocation(ProgramHandle(), "m_hdrPgsSaturation");
  m_sdrPgsPeak = glGetUniformLocation(ProgramHandle(), "m_sdrPgsPeak");
  m_sdrPgsSaturation = glGetUniformLocation(ProgramHandle(), "m_sdrPgsSaturation");
  m_guiSrgbDecode = glGetUniformLocation(ProgramHandle(), "m_guiSrgbDecode");
  m_guiDither8 = glGetUniformLocation(ProgramHandle(), "m_guiDither8");
  m_guiTransferBypass = glGetUniformLocation(ProgramHandle(), "m_guiTransferBypass");
  m_guiCompositeDither = glGetUniformLocation(ProgramHandle(), "m_guiCompositeDither");

  // Variables passed directly to the Vertex shader
  m_hProj  = glGetUniformLocation(ProgramHandle(), "m_proj");
  m_hModel = glGetUniformLocation(ProgramHandle(), "m_model");
  m_hCoord0Matrix = glGetUniformLocation(ProgramHandle(), "m_coord0Matrix");
  m_hMatrix = glGetUniformLocation(ProgramHandle(), "m_matrix");
  m_hShaderClip = glGetUniformLocation(ProgramHandle(), "m_shaderClip");
  m_hCoordStep = glGetUniformLocation(ProgramHandle(), "m_cordStep");
  m_hDepth = glGetUniformLocation(ProgramHandle(), "m_depth");

  m_hVertexBlock = glGetUniformBlockIndex(ProgramHandle(), "KodiGuiVertexBlock");
  if (m_hVertexBlock >= 0)
    glUniformBlockBinding(ProgramHandle(), static_cast<GLuint>(m_hVertexBlock),
                          GUI_VERTEX_BINDING_POINT);

  m_hFragmentBlock = glGetUniformBlockIndex(ProgramHandle(), "KodiGuiFragmentBlock");
  if (m_hFragmentBlock >= 0)
    glUniformBlockBinding(ProgramHandle(), static_cast<GLuint>(m_hFragmentBlock),
                          GUI_FRAGMENT_BINDING_POINT);

  // Vertex attributes
  if (KODI::GLES::UsesFixedAttributeLocationsForShader(VertexShader()->GetName()))
  {
    m_hPos = 0;
    m_hCol = 1;
    m_hCord0 = 2;
    m_hCord1 = 3;
  }
  else
  {
    m_hPos = glGetAttribLocation(ProgramHandle(), "m_attrpos");
    m_hCol = glGetAttribLocation(ProgramHandle(), "m_attrcol");
    m_hCord0 = glGetAttribLocation(ProgramHandle(), "m_attrcord0");
    m_hCord1 = glGetAttribLocation(ProgramHandle(), "m_attrcord1");
  }

  // It's okay to do this only one time. Textures units never change.
  glUseProgram( ProgramHandle() );
  glUniform1i(m_hTex0, 0);
  glUniform1i(m_hTex1, 1);
  glUniform4f(m_hUniCol, 1.0, 1.0, 1.0, 1.0);

  const float identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
  };
  glUniformMatrix4fv(m_hCoord0Matrix,  1, GL_FALSE, identity);

  glUseProgram( 0 );

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (settings)
  {
    settings->RegisterCallback(this, {
      CSettings::SETTING_VIDEOSCREEN_HDRPGSPEAKLUMINANCE,
      CSettings::SETTING_VIDEOSCREEN_HDRPGSSATURATION,
      CSettings::SETTING_VIDEOSCREEN_SDRPGSPEAKLUMINANCE,
      CSettings::SETTING_VIDEOSCREEN_SDRPGSSATURATION
    });
    m_cachedHdrPgsPeak = static_cast<float>(std::clamp(settings->GetInt(CSettings::SETTING_VIDEOSCREEN_HDRPGSPEAKLUMINANCE), 0, 100)) / 50.0f;
    m_cachedHdrPgsSaturation = static_cast<float>(std::clamp(settings->GetInt(CSettings::SETTING_VIDEOSCREEN_HDRPGSSATURATION), 0, 100)) / 50.0f;
    m_cachedSdrPgsPeak = static_cast<float>(std::clamp(settings->GetInt(CSettings::SETTING_VIDEOSCREEN_SDRPGSPEAKLUMINANCE), 0, 100)) / 50.0f;
    m_cachedSdrPgsSaturation = static_cast<float>(std::clamp(settings->GetInt(CSettings::SETTING_VIDEOSCREEN_SDRPGSSATURATION), 0, 100)) / 50.0f;
  }
}

CGLESShader::~CGLESShader()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (settingsComponent)
  {
    const auto settings = settingsComponent->GetSettings();
    if (settings) settings->UnregisterCallback(this);
  }

  Free();
}

void CGLESShader::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting == nullptr) return;

  const std::string& settingId = setting->GetId();

  if (settingId == CSettings::SETTING_VIDEOSCREEN_HDRPGSPEAKLUMINANCE)
  {
    const int hdrPgsPeakSetting = std::clamp(std::static_pointer_cast<const CSettingInt>(setting)->GetValue(), 0, 100);
    m_cachedHdrPgsPeak = static_cast<float>(hdrPgsPeakSetting) / 50.0f;
  }
  else if (settingId == CSettings::SETTING_VIDEOSCREEN_HDRPGSSATURATION)
  {
    const int hdrPgsSaturationSetting = std::clamp(std::static_pointer_cast<const CSettingInt>(setting)->GetValue(), 0, 100);
    m_cachedHdrPgsSaturation = static_cast<float>(hdrPgsSaturationSetting) / 50.0f;
  }
  else if (settingId == CSettings::SETTING_VIDEOSCREEN_SDRPGSPEAKLUMINANCE)
  {
    const int sdrPgsPeakSetting = std::clamp(std::static_pointer_cast<const CSettingInt>(setting)->GetValue(), 0, 100);
    m_cachedSdrPgsPeak = static_cast<float>(sdrPgsPeakSetting) / 50.0f;
  }
  else if (settingId == CSettings::SETTING_VIDEOSCREEN_SDRPGSSATURATION)
  {
    const int sdrPgsSaturationSetting = std::clamp(std::static_pointer_cast<const CSettingInt>(setting)->GetValue(), 0, 100);
    m_cachedSdrPgsSaturation = static_cast<float>(sdrPgsSaturationSetting) / 50.0f;
  }
}

namespace
{
std::atomic<float> g_frameGuiSdrPeak{1.0f};
std::atomic<float> g_frameGuiSdrSaturation{1.0f};
std::atomic<float> g_frameGuiSrgbDecode{0.0f};
std::atomic<float> g_frameGuiDither8{0.0f};
std::atomic<float> g_frameGuiCompositeDither{1.0f};
}

void CGLESShader::RefreshFrameGuiValues()
{
  const auto winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem)
    return;
  g_frameGuiSdrPeak.store(winSystem->GetGuiSdrPeakLuminance(), std::memory_order_relaxed);
  g_frameGuiSdrSaturation.store(winSystem->GetGuiSdrSaturation(), std::memory_order_relaxed);
  g_frameGuiSrgbDecode.store(winSystem->GetGuiSrgbDecode(), std::memory_order_relaxed);
  g_frameGuiDither8.store(winSystem->GetGuiDither8Bit(), std::memory_order_relaxed);
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  g_frameGuiCompositeDither.store((advSettings && advSettings->m_guiCompositeDither) ? 1.0f : 0.0f,
                                 std::memory_order_relaxed);
}

bool CGLESShader::OnEnabled()
{
  // This is called after glUseProgram()

  std::array<GLfloat, 16> projSnapshot;
  std::array<GLfloat, 16> modelSnapshot;
  const GLfloat* projLive = glMatrixProject.Get();
  const GLfloat* modelLive = glMatrixModview.Get();
  std::copy_n(projLive, 16, projSnapshot.begin());
  std::copy_n(modelLive, 16, modelSnapshot.begin());
  if (m_hVertexBlock >= 0)
  {
    const bool changed = !m_vertexBlockValid || m_vertexUBO == 0 ||
                         projSnapshot != m_lastProj || modelSnapshot != m_lastModel;
    if (changed)
    {
      GuiVertexBlockData vertexBlock;
      vertexBlock.proj = projSnapshot;
      vertexBlock.model = modelSnapshot;
      EnsureGuiUniformBuffer(m_vertexUBO, sizeof(GuiVertexBlockData));
      glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GuiVertexBlockData), &vertexBlock);
      glBindBuffer(GL_UNIFORM_BUFFER, 0);
      m_lastProj = projSnapshot;
      m_lastModel = modelSnapshot;
      m_vertexBlockValid = true;
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, GUI_VERTEX_BINDING_POINT, m_vertexUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }
  else
  {
    glUniformMatrix4fv(m_hProj, 1, GL_FALSE, projSnapshot.data());
    glUniformMatrix4fv(m_hModel, 1, GL_FALSE, modelSnapshot.data());
  }

  const TransformMatrix &guiMatrix = CServiceBroker::GetWinSystem()->GetGfxContext().GetGUIMatrix();
  CRect viewPort; // absolute positions of corners
  CServiceBroker::GetRenderSystem()->GetViewPort(viewPort);

  /* glScissor operates in window coordinates. In order that we can use it to
   * perform clipping, we must ensure that there is an independent linear
   * transformation from the coordinate system used by CGraphicContext::ClipRect
   * to window coordinates, separately for X and Y (in other words, no
   * rotation or shear is introduced at any stage). To do, this, we need to
   * check that zeros are present in the following locations:
   *
   * GUI matrix:
   * / * 0 * * \
   * | 0 * * * |
   * \ 0 0 * * /
   *       ^ TransformMatrix::TransformX/Y/ZCoord are only ever called with
   *         input z = 0, so this column doesn't matter
   * Model-view matrix:
   * / * 0 0 * \
   * | 0 * 0 * |
   * | 0 0 * * |
   * \ * * * * /  <- eye w has no influence on window x/y (last column below
   *                                                       is either 0 or ignored)
   * Projection matrix:
   * / * 0 0 0 \
   * | 0 * 0 0 |
   * | * * * * |  <- normalised device coordinate z has no influence on window x/y
   * \ 0 0 * 0 /
   *
   * Some of these zeros are not strictly required to ensure this, but they tend
   * to be zeroed in the common case, so by checking for zeros here, we simplify
   * the calculation of the window x/y coordinates further down the line.
   *
   * (Minor detail: we don't quite deal in window coordinates as defined by
   * OpenGL, because CRenderSystemGLES::SetScissors flips the Y axis. But all
   * that's needed to handle that is an effective negation at the stage where
   * Y is in normalised device coordinates.)
   */
  m_clipPossible = guiMatrix.m[0][1] == 0 &&
      guiMatrix.m[1][0] == 0 &&
      guiMatrix.m[2][0] == 0 &&
      guiMatrix.m[2][1] == 0 &&
      modelSnapshot[0+1*4] == 0 &&
      modelSnapshot[0+2*4] == 0 &&
      modelSnapshot[1+0*4] == 0 &&
      modelSnapshot[1+2*4] == 0 &&
      modelSnapshot[2+0*4] == 0 &&
      modelSnapshot[2+1*4] == 0 &&
      projSnapshot[0+1*4] == 0 &&
      projSnapshot[0+2*4] == 0 &&
      projSnapshot[0+3*4] == 0 &&
      projSnapshot[1+0*4] == 0 &&
      projSnapshot[1+2*4] == 0 &&
      projSnapshot[1+3*4] == 0 &&
      projSnapshot[3+0*4] == 0 &&
      projSnapshot[3+1*4] == 0 &&
      projSnapshot[3+3*4] == 0;

  m_clipXFactor = 0.0;
  m_clipXOffset = 0.0;
  m_clipYFactor = 0.0;
  m_clipYOffset = 0.0;

  if (m_clipPossible)
  {
    m_clipXFactor = guiMatrix.m[0][0] * modelSnapshot[0+0*4] * projSnapshot[0+0*4];
    m_clipXOffset = (guiMatrix.m[0][3] * modelSnapshot[0+0*4] + modelSnapshot[0+3*4]) * projSnapshot[0+0*4];
    m_clipYFactor = guiMatrix.m[1][1] * modelSnapshot[1+1*4] * projSnapshot[1+1*4];
    m_clipYOffset = (guiMatrix.m[1][3] * modelSnapshot[1+1*4] + modelSnapshot[1+3*4]) * projSnapshot[1+1*4];
    float clipW = (guiMatrix.m[2][3] * modelSnapshot[2+2*4] + modelSnapshot[2+3*4]) * projSnapshot[3+2*4];
    float xMult = (viewPort.x2 - viewPort.x1) / (2 * clipW);
    float yMult = (viewPort.y1 - viewPort.y2) / (2 * clipW); // correct for inverted window coordinate scheme
    m_clipXFactor = m_clipXFactor * xMult;
    m_clipXOffset = m_clipXOffset * xMult + (viewPort.x2 + viewPort.x1) / 2;
    m_clipYFactor = m_clipYFactor * yMult;
    m_clipYOffset = m_clipYOffset * yMult + (viewPort.y2 + viewPort.y1) / 2;
  }

  const float currentSdrPeak = g_frameGuiSdrPeak.load(std::memory_order_relaxed);
  const float currentSdrSaturation = g_frameGuiSdrSaturation.load(std::memory_order_relaxed);
  const float currentSrgbDecode = g_frameGuiSrgbDecode.load(std::memory_order_relaxed);
  const float currentDither8 = g_frameGuiDither8.load(std::memory_order_relaxed);
  const float currentCompositeDither = g_frameGuiCompositeDither.load(std::memory_order_relaxed);

  if (m_hFragmentBlock >= 0)
  {
    GuiFragmentBlockData fragmentBlock;
    fragmentBlock.guiParams0 = {0.0f, 1.0f, currentSdrPeak, currentSdrSaturation};
    fragmentBlock.guiParams1 = {m_cachedHdrPgsPeak, m_cachedHdrPgsSaturation, m_cachedSdrPgsPeak, m_cachedSdrPgsSaturation};
    if (!m_fragmentBlockValid || m_fragmentUBO == 0 ||
        fragmentBlock.guiParams0 != m_lastGuiParams0 ||
        fragmentBlock.guiParams1 != m_lastGuiParams1)
    {
      EnsureGuiUniformBuffer(m_fragmentUBO, sizeof(GuiFragmentBlockData));
      glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GuiFragmentBlockData), &fragmentBlock);
      glBindBuffer(GL_UNIFORM_BUFFER, 0);
      m_lastGuiParams0 = fragmentBlock.guiParams0;
      m_lastGuiParams1 = fragmentBlock.guiParams1;
      m_fragmentBlockValid = true;
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, GUI_FRAGMENT_BINDING_POINT, m_fragmentUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }
  else
  {
    glUniform1f(m_hBrightness, 0.0f);
    glUniform1f(m_hContrast, 1.0f);

    if (m_sdrPeak >= 0) glUniform1f(m_sdrPeak, currentSdrPeak);
    if (m_sdrSaturation >= 0) glUniform1f(m_sdrSaturation, currentSdrSaturation);
    if (m_hdrPgsPeak >= 0) glUniform1f(m_hdrPgsPeak, m_cachedHdrPgsPeak);
    if (m_hdrPgsSaturation >= 0) glUniform1f(m_hdrPgsSaturation, m_cachedHdrPgsSaturation);
    if (m_sdrPgsPeak >= 0) glUniform1f(m_sdrPgsPeak, m_cachedSdrPgsPeak);
    if (m_sdrPgsSaturation >= 0) glUniform1f(m_sdrPgsSaturation, m_cachedSdrPgsSaturation);
  }

  if (m_guiSrgbDecode >= 0)
    glUniform1f(m_guiSrgbDecode, currentSrgbDecode);
  if (m_guiDither8 >= 0)
    glUniform1f(m_guiDither8, currentDither8);
  if (m_guiCompositeDither >= 0)
    glUniform1f(m_guiCompositeDither, currentCompositeDither);

  static float s_lastSdrPgsPeak = -1.0f;
  static float s_lastSdrPgsSaturation = -1.0f;
  if (m_cachedSdrPgsPeak != s_lastSdrPgsPeak ||
      m_cachedSdrPgsSaturation != s_lastSdrPgsSaturation)
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "uniforms updated: sdrPgsPeak={} sdrPgsSat={} hdrPgsPeak={} hdrPgsSat={} guiSdrPeak={} guiSdrSat={} hasFragmentBlock={} sdrPgsPeakLoc={} sdrPgsSatLoc={}",
                  m_cachedSdrPgsPeak, m_cachedSdrPgsSaturation, m_cachedHdrPgsPeak, m_cachedHdrPgsSaturation,
                  currentSdrPeak, currentSdrSaturation, m_hFragmentBlock >= 0, m_sdrPgsPeak,
                  m_sdrPgsSaturation);
    s_lastSdrPgsPeak = m_cachedSdrPgsPeak;
    s_lastSdrPgsSaturation = m_cachedSdrPgsSaturation;
  }

  return true;
}

void CGLESShader::Free()
{
  if (m_vertexUBO != 0)
  {
    glDeleteBuffers(1, &m_vertexUBO);
    m_vertexUBO = 0;
  }

  if (m_fragmentUBO != 0)
  {
    glDeleteBuffers(1, &m_fragmentUBO);
    m_fragmentUBO = 0;
  }

  m_hVertexBlock = -1;
  m_hFragmentBlock = -1;
  m_vertexBlockValid = false;
  m_fragmentBlockValid = false;

  CGLSLShaderProgram::Free();
}
