/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace KODI
{
namespace VIDEORENDERER
{

struct ActiveAreaScanContext;

class CActiveAreaDetector
{
public:
  CActiveAreaDetector();
  ~CActiveAreaDetector();

  CActiveAreaDetector(const CActiveAreaDetector&) = delete;
  CActiveAreaDetector& operator=(const CActiveAreaDetector&) = delete;

  void Start(const std::string& filePath, bool runInitialScan = false, bool startSuspended = false);
  void Stop();

  void SetManualAspect(const std::string& aspect);
  void SetPeriodicRescan(bool enabled, int intervalSec);
  void SetSuspended(bool suspended);
  void SetL5Available(bool available);
  void NotifyL5Transition(bool available, uint16_t topPx, uint16_t bottomPx);
  void NotifyBitmapOverlaySeen();

  bool TryGetActiveArea(int frameWidth, int frameHeight, int& topPx, int& bottomPx) const;
  bool IsStable() const { return m_stable.load(std::memory_order_acquire); }

private:
  void ThreadProc();
  bool RunOneScan(bool periodic, bool l5Transition = false);
  void RunInitialScanIfNeeded();
  bool ApplyManualAspect(int frameWidth, int frameHeight, int& topPx, int& bottomPx) const;

  std::thread m_thread;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;

  std::string m_filePath;
  std::string m_manualAspect{"auto"};

  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_suspended{false};
  bool m_initialScanPending{false};
  std::atomic<uint32_t> m_overlayGen{0};
  uint32_t m_overlayGenSeen{0};
  bool m_runInitialScan{false};
  std::atomic<bool> m_stable{false};

  std::atomic<uint64_t> m_detectedArea{0};

  std::atomic<bool> m_periodicEnabled{true};
  std::atomic<int> m_periodicIntervalSec{4};
  int m_stableMatchCount{0};
  std::atomic<bool> m_l5Available{false};
  std::atomic<bool> m_everL5{false};
  std::atomic<uint64_t> m_l5LastState{~0ULL};
  std::atomic<int> m_l5TransitionScans{0};
  std::chrono::steady_clock::time_point m_lastScanTime{};
  int m_scanRetryOffset{0};
  int m_consecFailedScans{0};
  uint16_t m_pendingTopPx{0};
  uint16_t m_pendingBottomPx{0};
  bool m_havePending{false};
  bool m_variableAR{false};
  int m_pendingMatchCount{0};
  std::atomic<bool> m_abort{false};
  std::unique_ptr<ActiveAreaScanContext> m_scanCtx;
};

}
}
