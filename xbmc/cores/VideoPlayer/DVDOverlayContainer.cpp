/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDOverlayContainer.h"

#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"

#include <memory>
#include <mutex>

CDVDOverlayContainer::~CDVDOverlayContainer()
{
  Clear();
}

void CDVDOverlayContainer::ProcessAndAddOverlayIfValid(const std::shared_ptr<CDVDOverlay>& pOverlay)
{
  std::unique_lock<CCriticalSection> lock(*this);

  if (pOverlay->IsDiscMenuOverlay())
  {
    for (auto it = m_overlays.begin(); it != m_overlays.end();)
    {
      if ((*it)->IsDiscMenuOverlay())
        it = m_overlays.erase(it);
      else
        ++it;
    }
  }

  // markup any non ending overlays, to finish
  // when this new one starts, there can be
  // multiple overlays queued at same start
  // point so only stop them when we get a
  // new startpoint
  if (pOverlay->iPTSStartTime >= 0)
  {
    for(int i = m_overlays.size();i>0;)
    {
      i--;
      if(m_overlays[i]->iPTSStopTime)
      {
        if(!m_overlays[i]->replace)
          break;
        if(m_overlays[i]->iPTSStopTime <= pOverlay->iPTSStartTime)
          break;
      }

      if (m_overlays[i]->IsDiscMenuOverlay())
        continue;

      if (m_overlays[i]->iPTSStartTime != pOverlay->iPTSStartTime)
        m_overlays[i]->iPTSStopTime = pOverlay->iPTSStartTime;
    }
  }

  m_overlays.emplace_back(pOverlay);
  m_size.store(m_overlays.size(), std::memory_order_relaxed);

  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                "overlay container add: startT={:.6f} stopT={:.6f} forced={} "
                "replace={} isGroup={} size={}",
                pOverlay->iPTSStartTime / DVD_TIME_BASE,
                pOverlay->iPTSStopTime / DVD_TIME_BASE, pOverlay->bForced,
                pOverlay->replace,
                pOverlay->IsOverlayType(DVDOVERLAY_TYPE_GROUP), m_overlays.size());
}

VecOverlays* CDVDOverlayContainer::GetOverlays()
{
  return &m_overlays;
}

VecOverlays::iterator CDVDOverlayContainer::Remove(VecOverlays::iterator itOverlay)
{
  std::unique_lock<CCriticalSection> lock(*this);
  auto it = m_overlays.erase(itOverlay);
  m_size.store(m_overlays.size(), std::memory_order_relaxed);
  return it;
}

void CDVDOverlayContainer::CleanUp(double pts)
{
  std::unique_lock<CCriticalSection> lock(*this);

  auto it = m_overlays.begin();
  while (it != m_overlays.end())
  {
    const std::shared_ptr<CDVDOverlay>& pOverlay = *it;

    // never delete forced overlays, they are used in menu's
    // clear takes care of removing them
    // also if stoptime = 0, it means the next subtitles will use its starttime as the stoptime
    // which means we cannot delete overlays with stoptime 0
    if (!pOverlay->bForced && pOverlay->iPTSStopTime <= pts && pOverlay->iPTSStopTime != 0)
    {
      //CLog::Log(LOGDEBUG,"CDVDOverlay::CleanUp, removing {}", (int)(pts / 1000));
      //CLog::Log(LOGDEBUG,"CDVDOverlay::CleanUp, remove, start : {}, stop : {}", (int)(pOverlay->iPTSStartTime / 1000), (int)(pOverlay->iPTSStopTime / 1000));
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "overlay container remove: startT={:.6f} stopT={:.6f} pts={:.6f}",
                    pOverlay->iPTSStartTime / DVD_TIME_BASE,
                    pOverlay->iPTSStopTime / DVD_TIME_BASE, pts / DVD_TIME_BASE);
      it = Remove(it);
      continue;
    }
    else if (pOverlay->bForced)
    {
      //Check for newer replacements
      auto it2 = it;
      bool bNewer = false;
      while (!bNewer && ++it2 != m_overlays.end())
      {
        const std::shared_ptr<CDVDOverlay>& pOverlay2 = *it2;
        // There can be multiple overlays queued at same start point.
        // Skip them to find a new start point.
        if (pOverlay2->bForced && pOverlay2->iPTSStartTime <= pts)
          bNewer = true;
      }

      if (bNewer)
      {
        it = Remove(it);
        continue;
      }
    }
    ++it;
  }

}

void CDVDOverlayContainer::Flush()
{
  std::unique_lock<CCriticalSection> lock(*this);

  // Flush only the overlays marked as flushable
  const size_t before = m_overlays.size();
  size_t discMenu = 0;
  for (const auto& ov : m_overlays)
  {
    if (ov->IsDiscMenuOverlay())
      ++discMenu;
  }

  m_overlays.erase(std::remove_if(m_overlays.begin(), m_overlays.end(),
                                  [](const std::shared_ptr<CDVDOverlay>& ov) {
                                    return ov->IsOverlayContainerFlushable();
                                  }),
                   m_overlays.end());
  m_size.store(m_overlays.size(), std::memory_order_relaxed);

  logComponentM(LOGDEBUG, LOGVIDEO, "overlay container Flush: size {} -> {} (discMenu entries before={})",
                before, m_overlays.size(), discMenu);
}

void CDVDOverlayContainer::Clear()
{
  std::unique_lock<CCriticalSection> lock(*this);
  const size_t before = m_overlays.size();
  m_overlays.clear();
  m_size.store(0, std::memory_order_relaxed);

  logComponentM(LOGDEBUG, LOGVIDEO, "overlay container Clear: size {} -> 0", before);
}

size_t CDVDOverlayContainer::GetSize() const {
  return m_size.load(std::memory_order_relaxed);
}

bool CDVDOverlayContainer::ContainsOverlayType(DVDOverlayType type)
{
  bool result = false;

  std::unique_lock<CCriticalSection> lock(*this);

  auto it = m_overlays.begin();
  while (!result && it != m_overlays.end())
  {
    if ((*it)->IsOverlayType(type)) result = true;
    ++it;
  }

  return result;
}

/*
 * iAction should be LIBDVDNAV_BUTTON_NORMAL or LIBDVDNAV_BUTTON_CLICKED
 */
void CDVDOverlayContainer::UpdateOverlayInfo(
    const std::shared_ptr<CDVDInputStreamNavigator>& pStream, CDVDDemuxSPU* pSpu, int iAction)
{
  std::unique_lock<CCriticalSection> lock(*this);

  pStream->CheckButtons();

  //Update any forced overlays.
  for(auto it = m_overlays.begin(); it != m_overlays.end(); ++it )
  {
    if ((*it)->IsOverlayType(DVDOVERLAY_TYPE_SPU))
    {
      auto pOverlaySpu = std::static_pointer_cast<CDVDOverlaySpu>(*it);

      // make sure its a forced (menu) overlay
      // set menu spu color and alpha data if there is a valid menu overlay
      if (pOverlaySpu->bForced)
      {
        if (pOverlaySpu.use_count() > 1)
        {
          pOverlaySpu = std::make_shared<CDVDOverlaySpu>(*pOverlaySpu);
          (*it) = pOverlaySpu;
        }

        if (pStream->GetCurrentButtonInfo(*pOverlaySpu, pSpu, iAction))
        {
          pOverlaySpu->m_textureid = 0;
        }

      }
    }
  }
}
