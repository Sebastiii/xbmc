/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "RenderSystemTypes.h"
#include "utils/ColorUtils.h"
#include "utils/Geometry.h"

#include <memory>
#include <string>
#include <vector>

/*
 *   CRenderSystemBase interface allows us to create the rendering engine we use.
 *   We currently have two engines: OpenGL and DirectX
 *   This interface is very basic since a lot of the actual details will go in to the derived classes
 */

enum DEPTH_CULLING
{
  DEPTH_CULLING_OFF = 0,
  DEPTH_CULLING_BACK_TO_FRONT,
  DEPTH_CULLING_FRONT_TO_BACK,
};

class CGUIRenderTargetFBO
{
public:
  static constexpr size_t MAX_CONTENT_RECTS = 8;

  virtual ~CGUIRenderTargetFBO() = default;

  virtual unsigned int GetWidth() const = 0;
  virtual unsigned int GetHeight() const = 0;

  void SetContentRects(std::vector<CRect> rects) { m_contentRects = std::move(rects); }
  const std::vector<CRect>& GetContentRects() const { return m_contentRects; }

private:
  std::vector<CRect> m_contentRects;
};

class CGUIImage;
class CGUITextLayout;

class CRenderSystemBase
{
public:
  CRenderSystemBase();
  virtual ~CRenderSystemBase();

  virtual bool InitRenderSystem() = 0;
  virtual bool DestroyRenderSystem() = 0;
  virtual bool ResetRenderSystem(int width, int height) = 0;

  virtual bool BeginRender() = 0;
  virtual bool EndRender() = 0;
  virtual void PresentRender(bool rendered, bool videoLayer) = 0;
  virtual bool SupportsGuiRenderTargets() const;
  virtual std::unique_ptr<CGUIRenderTargetFBO> CreateGuiRenderTarget(unsigned int width,
                                                                  unsigned int height);
  virtual bool BeginGuiRenderTarget(CGUIRenderTargetFBO& target);
  virtual bool BeginGuiRenderTargetPersistent(CGUIRenderTargetFBO& target, bool clearColor);
  virtual bool SupportsGuiRenderTargetConvert() const { return false; }
  virtual void EndGuiRenderTarget(CGUIRenderTargetFBO& target);
  virtual bool RenderGuiRenderTarget(const CGUIRenderTargetFBO& target, bool replace = false);
  virtual void* CreateGuiRenderFence();
  virtual bool WaitGuiRenderFence(void* fence, bool poll);
  virtual bool WaitGuiRenderFenceBounded(void* fence, uint64_t maxWaitNs);
  virtual void DeleteGuiRenderFence(void* fence);
  virtual bool SupportsGuiRenderTimer() const { return false; }
  virtual void BeginGuiRenderTimer() {}
  virtual void EndGuiRenderTimer() {}
  virtual bool PollGuiRenderTimerNs(uint64_t& elapsedNs) { return false; }
  virtual void EstablishGuiRenderBaseline(unsigned int width, unsigned int height);
  virtual void SetThreadGuiShaderScope(bool worker) {}
  virtual void ReleaseThreadGuiShaders() {}
  virtual unsigned int GetGuiShaderEpoch() const { return 0; }
  virtual void InvalidateColorBuffer() {}
  virtual bool ClearBuffers(UTILS::COLOR::Color color) = 0;
  virtual bool IsExtSupported(const char* extension) const = 0;

  virtual void SetViewPort(const CRect& viewPort) = 0;
  virtual void GetViewPort(CRect& viewPort) = 0;
  virtual void RestoreViewPort() {}

  virtual bool ScissorsCanEffectClipping() { return false; }
  virtual CRect ClipRectToScissorRect(const CRect &rect) { return CRect(); }
  virtual void SetScissors(const CRect &rect) = 0;
  virtual void ResetScissors() = 0;

  virtual void SetDepthCulling(DEPTH_CULLING culling) {}

  virtual void CaptureStateBlock() = 0;
  virtual void ApplyStateBlock() = 0;

  virtual void SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor = 0.f) = 0;
  virtual void SetStereoMode(RENDER_STEREO_MODE mode, RENDER_STEREO_VIEW view)
  {
    m_stereoMode = mode;
    m_stereoView = view;
  }

  /**
   * Project (x,y,z) 3d scene coordinates to (x,y) 2d screen coordinates
   */
  virtual void Project(float &x, float &y, float &z) { }

  virtual std::string GetShaderPath(const std::string &filename) { return ""; }

  void GetRenderVersion(unsigned int& major, unsigned int& minor) const;
  const std::string& GetRenderVendor() const { return m_RenderVendor; }
  const std::string& GetRenderRenderer() const { return m_RenderRenderer; }
  const std::string& GetRenderVersionString() const { return m_RenderVersion; }
  virtual bool SupportsNPOT(bool dxt) const;
  virtual bool SupportsStereo(RENDER_STEREO_MODE mode) const;
  unsigned int GetMaxTextureSize() const { return m_maxTextureSize; }
  unsigned int GetMinDXTPitch() const { return m_minDXTPitch; }

  virtual void ShowSplash(const std::string& message);

protected:
  bool                m_bRenderCreated;
  bool                m_bVSync;
  unsigned int        m_maxTextureSize;
  unsigned int        m_minDXTPitch;

  std::string   m_RenderRenderer;
  std::string   m_RenderVendor;
  std::string   m_RenderVersion;
  int          m_RenderVersionMinor;
  int          m_RenderVersionMajor;
  RENDER_STEREO_VIEW m_stereoView = RENDER_STEREO_VIEW_OFF;
  RENDER_STEREO_MODE m_stereoMode = RENDER_STEREO_MODE_OFF;
  bool m_limitedColorRange = false;

  std::unique_ptr<CGUIImage> m_splashImage;
  std::unique_ptr<CGUITextLayout> m_splashMessageLayout;
};

