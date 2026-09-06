/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/VideoSync.h"
#include "guilib/DispResource.h"

#include <chrono>
#include <cstdint>

class CVideoSyncAML : public CVideoSync, IDispResource
{
public:
  CVideoSyncAML(CVideoReferenceClock *clock);
  virtual ~CVideoSyncAML();
  virtual bool Setup()override;
  virtual void Run(CEvent& stopEvent)override;
  virtual void Cleanup()override;
  virtual float GetFps()override;
  virtual void OnResetDisplay()override;
private:
  volatile bool m_abort;
  int m_fbFd{-1};
  int64_t m_lastKernelTs{0};
  int64_t m_cntKernelTsHits{0};
  int64_t m_cntKernelTsZero{0};
  int64_t m_cntKernelTsUnchanged{0};
  int64_t m_cntIoctlError{0};
  int64_t m_cntFpsResets{0};
  int64_t m_cntLegacySignaled{0};
  int64_t m_cntLegacyTimeout{0};

  std::chrono::steady_clock::time_point m_lastGoodTs{};
  std::chrono::steady_clock::time_point m_lastProbe{};
  bool m_vsyncDegraded{false};
  int m_failedProbes{0};
  int64_t m_stallTs{0};
  bool m_stallFaultLogged{false};
  bool m_legacyLatched{false};
  bool m_fallbackOnStall{false};
};
