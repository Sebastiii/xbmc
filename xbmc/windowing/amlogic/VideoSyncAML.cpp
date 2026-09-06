/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "ServiceBroker.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "windowing/GraphicContext.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "threads/Thread.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC_64
#define FBIO_WAITFORVSYNC_64 _IOW('F', 0x21, unsigned int)
#endif

extern CEvent g_aml_sync_event;

CVideoSyncAML::CVideoSyncAML(CVideoReferenceClock *clock)
: CVideoSync(clock)
, m_abort(false)
{
}

CVideoSyncAML::~CVideoSyncAML()
{
}

bool CVideoSyncAML::Setup()
{
  m_abort = false;
  m_lastKernelTs = 0;
  m_cntKernelTsHits = 0;
  m_cntKernelTsZero = 0;
  m_cntKernelTsUnchanged = 0;
  m_cntIoctlError = 0;
  m_cntFpsResets = 0;
  m_cntLegacySignaled = 0;
  m_cntLegacyTimeout = 0;
  m_lastGoodTs = {};
  m_lastProbe = {};
  m_vsyncDegraded = false;
  m_failedProbes = 0;
  m_stallTs = 0;
  m_stallFaultLogged = false;
  m_legacyLatched = false;

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  m_fallbackOnStall = settings &&
    settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_VIDEOSYNC_FALLBACK_ON_STALL);

  CServiceBroker::GetWinSystem()->Register(this);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up AML");

  m_fbFd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (m_fbFd < 0)
  {
    logM(LOGINFO,
         "CVideoReferenceClock: unable to open /dev/fb0 for vsync ({}), "
         "falling back to legacy codec event",
         strerror(errno));
  }
  else
  {
    logM(LOGDEBUG, "CVideoReferenceClock: using FBIO_WAITFORVSYNC_64 on /dev/fb0");
  }

  return true;
}

void CVideoSyncAML::Run(CEvent& stopEvent)
{
  auto startTs = std::chrono::steady_clock::now();
  uint64_t numVBlanks = 0;
  m_lastGoodTs = startTs;

  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  double last_fps = 0.0;
  double cur_fps = 0.0;
  int fpsPollCountdown = 0;
  double frameIntervalUs = 1'000'000.0 / 60.0;
  int64_t expectedIntervalNs = static_cast<int64_t>(1'000'000'000.0 / 60.0);
  auto legacyTimeout = std::chrono::microseconds(
      static_cast<int64_t>(std::max(8000.0, 3.0 * frameIntervalUs)));

  while (!stopEvent.Signaled() && !m_abort)
  {
    if (--fpsPollCountdown <= 0)
    {
      fpsPollCountdown = 4;
      cur_fps = static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS());
    }
    if (cur_fps != last_fps && cur_fps > 1.0)
    {
      frameIntervalUs = 1'000'000.0 / cur_fps;
      expectedIntervalNs = static_cast<int64_t>(1'000'000'000.0 / cur_fps);
      legacyTimeout = std::chrono::microseconds(
          static_cast<int64_t>(std::max(8000.0, 3.0 * frameIntervalUs)));
      startTs = std::chrono::steady_clock::now();
      numVBlanks = 0;
      m_lastKernelTs = 0;
      m_lastGoodTs = startTs;
      m_vsyncDegraded = false;
      m_failedProbes = 0;
      m_stallFaultLogged = false;
      if (last_fps > 0.0)
      {
        logM(LOGDEBUG,
             "CVideoSyncAML: fps changed {:.3f} -> {:.3f}, reset clock",
             last_fps, cur_fps);
        m_refClock->UpdateRefreshrate();
        m_cntFpsResets++;
      }
      last_fps = cur_fps;
    }

    const auto nowSteady = std::chrono::steady_clock::now();
    const bool useVsyncPath = (m_fbFd >= 0) && !m_legacyLatched;

    if (useVsyncPath && !m_vsyncDegraded)
    {
      int64_t kernelTs = 0;
      if (ioctl(m_fbFd, FBIO_WAITFORVSYNC_64, &kernelTs) == 0)
      {
        if (kernelTs > 0 && kernelTs != m_lastKernelTs)
        {
          int countVSyncs = 1;
          int64_t deltaNs = 0;
          if (m_lastKernelTs > 0 && expectedIntervalNs > 0)
          {
            deltaNs = kernelTs - m_lastKernelTs;
            countVSyncs = static_cast<int>(
                (deltaNs + expectedIntervalNs / 2) / expectedIntervalNs);
            if (countVSyncs < 1)
              countVSyncs = 1;
          }
          if (countVSyncs > 1)
          {
            static auto s_lastLogged_caughtUp = std::chrono::steady_clock::time_point{};
            auto nowLog = std::chrono::steady_clock::now();
            if (nowLog - s_lastLogged_caughtUp >= std::chrono::seconds(1))
            {
              s_lastLogged_caughtUp = nowLog;
              logComponentM(LOGDEBUG, LOGAVTIMING,
                            "CVideoSyncAML: caught up {} vsyncs (deltaNs={}, "
                            "expectedNs={})",
                            countVSyncs, deltaNs, expectedIntervalNs);
            }
          }
          m_lastKernelTs = kernelTs;
          m_lastGoodTs = nowSteady;
          numVBlanks += static_cast<uint64_t>(countVSyncs);
          m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());
          m_cntKernelTsHits++;
          continue;
        }
        if (kernelTs == 0)
        {
          m_cntKernelTsZero++;
          static auto s_lastLogged_tsZero = std::chrono::steady_clock::time_point{};
          auto nowLog = std::chrono::steady_clock::now();
          if (nowLog - s_lastLogged_tsZero >= std::chrono::seconds(1))
          {
            s_lastLogged_tsZero = nowLog;
            logComponentM(LOGDEBUG, LOGAVTIMING,
                          "CVideoSyncAML: ioctl returned ts=0 (pre-first-vsync?), legacy fallback");
          }
        }
        else
        {
          m_cntKernelTsUnchanged++;
          static auto s_lastLogged_tsUnchanged = std::chrono::steady_clock::time_point{};
          auto nowLog = std::chrono::steady_clock::now();
          if (nowLog - s_lastLogged_tsUnchanged >= std::chrono::seconds(1))
          {
            s_lastLogged_tsUnchanged = nowLog;
            logComponentM(LOGDEBUG, LOGAVTIMING,
                          "CVideoSyncAML: ioctl ts unchanged ({}), legacy fallback", kernelTs);
          }
        }
        m_lastKernelTs = 0;

        const auto stnow = std::chrono::steady_clock::now();
        const auto stallThreshold = std::chrono::microseconds(
            static_cast<int64_t>(std::max(750000.0, 6.0 * frameIntervalUs)));
        if ((stnow - m_lastGoodTs) > stallThreshold)
        {
          logM(LOGDEBUG,
               "vsync gap {} ms - riding legacy timing, re-probing for recovery",
               std::chrono::duration_cast<std::chrono::milliseconds>(stnow - m_lastGoodTs).count());
          m_vsyncDegraded = true;
          m_failedProbes = 0;
          m_stallTs = kernelTs;
          m_lastProbe = stnow;
        }
        startTs = stnow;
        numVBlanks = 0;
      }
      else
      {
        if (errno == EINTR)
          continue;
        m_cntIoctlError++;
        m_lastKernelTs = 0;
        if (errno == ENOTTY || errno == EINVAL)
        {
          logM(LOGINFO,
               "CVideoReferenceClock: FBIO_WAITFORVSYNC_64 unsupported ({}), "
               "permanently falling back to legacy path",
               strerror(errno));
          close(m_fbFd);
          m_fbFd = -1;
          startTs = std::chrono::steady_clock::now();
          numVBlanks = 0;
        }
      }
    }
    else if (useVsyncPath && m_vsyncDegraded)
    {
      constexpr auto kProbeInterval = std::chrono::seconds(2);
      if (nowSteady - m_lastProbe >= kProbeInterval)
      {
        int64_t probeTs = 0;
        const bool ok = ioctl(m_fbFd, FBIO_WAITFORVSYNC_64, &probeTs) == 0;
        m_lastProbe = std::chrono::steady_clock::now();
        if (ok && probeTs > 0 && probeTs > m_stallTs)
        {
          logM(m_stallFaultLogged ? LOGWARNING : LOGINFO,
               "vsync recovered after {} ms - resuming kernel vsync clock",
               std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - m_lastGoodTs).count());
          m_vsyncDegraded = false;
          m_failedProbes = 0;
          m_stallFaultLogged = false;
          m_lastKernelTs = 0;
          m_lastGoodTs = m_lastProbe;
        }
        else
        {
          constexpr auto kFaultThreshold = std::chrono::seconds(10);
          if (!m_stallFaultLogged && (nowSteady - m_lastGoodTs) > kFaultThreshold)
          {
            logM(LOGWARNING,
                 "vsync stalled for {} ms (beyond any plausible mode switch)",
                 std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - m_lastGoodTs).count());
            m_stallFaultLogged = true;
          }
          if (m_fallbackOnStall && m_stallFaultLogged && ++m_failedProbes >= 3)
          {
            logM(LOGWARNING,
                 "vsync stall sustained - latching legacy timing for the rest of this playback");
            m_legacyLatched = true;
            CGUIDialogKaiToast::QueueNotification(
                CGUIDialogKaiToast::Warning,
                g_localizeStrings.Get(60112),
                g_localizeStrings.Get(60114),
                8000);
          }
        }
      }
    }

    int countVSyncs = 1;
    if (!g_aml_sync_event.Wait(legacyTimeout))
    {
      m_cntLegacyTimeout++;
      const auto elapsed = std::chrono::steady_clock::now() - startTs;
      const double elapsedUs =
          std::chrono::duration<double, std::micro>(elapsed).count();

      const double expected = elapsedUs / frameIntervalUs;
      uint64_t curVBlanks = static_cast<uint64_t>(expected);

      const double nextBoundaryUs = (curVBlanks + 1) * frameIntervalUs;
      if (nextBoundaryUs > elapsedUs)
      {
        const int64_t sleepUs = static_cast<int64_t>(nextBoundaryUs - elapsedUs);
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        ++curVBlanks;
      }

      if (curVBlanks > numVBlanks)
      {
        countVSyncs = static_cast<int>(curVBlanks - numVBlanks);
        numVBlanks = curVBlanks;
      }
      else
      {
        countVSyncs = 0;
      }
    }
    else
    {
      ++numVBlanks;
      m_cntLegacySignaled++;
    }

    if (countVSyncs > 0)
      m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());

    if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
        CServiceBroker::GetLogging().CanLogComponent(LOGAVTIMING))
    {
      static auto s_lastLogged_summary = std::chrono::steady_clock::time_point{};
      auto nowLog = std::chrono::steady_clock::now();
      if (nowLog - s_lastLogged_summary >= std::chrono::seconds(5))
      {
        s_lastLogged_summary = nowLog;
        logComponentM(LOGDEBUG, LOGAVTIMING,
                      "CVideoSyncAML: summary kernelTs hits={} zero={} unchanged={} "
                      "ioctlErr={} fpsResets={} legacy signaled={} timeout={}",
                      m_cntKernelTsHits, m_cntKernelTsZero, m_cntKernelTsUnchanged,
                      m_cntIoctlError, m_cntFpsResets, m_cntLegacySignaled,
                      m_cntLegacyTimeout);
      }
    }
  }
}

void CVideoSyncAML::Cleanup()
{
  logM(LOGDEBUG,
       "CVideoSyncAML: final summary kernelTs hits={} zero={} unchanged={} "
       "ioctlErr={} fpsResets={} legacy signaled={} timeout={}",
       m_cntKernelTsHits, m_cntKernelTsZero, m_cntKernelTsUnchanged,
       m_cntIoctlError, m_cntFpsResets, m_cntLegacySignaled, m_cntLegacyTimeout);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up AML");
  if (m_fbFd >= 0)
  {
    close(m_fbFd);
    m_fbFd = -1;
  }
  CServiceBroker::GetWinSystem()->Unregister(this);
}

float CVideoSyncAML::GetFps()
{
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: fps: {:.3f}", m_fps);
  return m_fps;
}

void CVideoSyncAML::OnResetDisplay()
{
  m_abort = true;
}
