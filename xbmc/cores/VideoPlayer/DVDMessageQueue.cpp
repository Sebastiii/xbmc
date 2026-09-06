/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDMessageQueue.h"

#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/SystemClock.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"

#include <limits>
#include <math.h>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

CDVDMessageQueue::CDVDMessageQueue(const std::string &owner) : m_hEvent(true), m_owner(owner)
{
  m_iDataSize     = 0;
  m_bInitialized = false;

  m_TimeBack = DVD_NOPTS_VALUE;
  m_TimeFront = DVD_NOPTS_VALUE;
  m_TimeSize = 1.0 / 4.0; /* 4 seconds */
  m_iMaxDataSize = 0;
}

CDVDMessageQueue::~CDVDMessageQueue()
{
  // remove all remaining messages
  Flush(CDVDMsg::NONE);
}

void CDVDMessageQueue::Init()
{
  m_iDataSize = 0;
  m_bAbortRequest = false;
  m_bInitialized = true;
  m_TimeBack = DVD_NOPTS_VALUE;
  m_TimeFront = DVD_NOPTS_VALUE;
  m_drain = false;
}

void CDVDMessageQueue::Flush(CDVDMsg::Message type)
{
  std::lock_guard lock(m_section);

  m_messages.remove_if([type](const DVDMessageListItem &item){
    return type == CDVDMsg::NONE || item.message->IsType(type);
  });

  m_prioMessages.remove_if([type](const DVDMessageListItem &item){
    return type == CDVDMsg::NONE || item.message->IsType(type);
  });

  if (type == CDVDMsg::DEMUXER_PACKET ||  type == CDVDMsg::NONE)
  {
    m_iDataSize = 0;
    m_TimeBack = DVD_NOPTS_VALUE;
    m_TimeFront = DVD_NOPTS_VALUE;
  }
}

void CDVDMessageQueue::Abort()
{
  std::lock_guard lock(m_section);

  m_bAbortRequest = true;

  // inform waiter for abort action
  m_hEvent.Set();
}

void CDVDMessageQueue::End()
{
  std::lock_guard lock(m_section);

  Flush(CDVDMsg::NONE);

  m_bInitialized = false;
  m_iDataSize = 0;
  m_bAbortRequest = false;
}

MsgQueueReturnCode CDVDMessageQueue::Put(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  return Put(pMsg, priority, true);
}

MsgQueueReturnCode CDVDMessageQueue::PutBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  return Put(pMsg, priority, false);
}

MsgQueueReturnCode CDVDMessageQueue::Put(const std::shared_ptr<CDVDMsg>& pMsg,
                                         int priority,
                                         bool front)
{
  std::unique_lock lock(m_section);

  if (!m_bInitialized)
  {
    LOG_THROTTLE_PERIODIC_GENERAL(LOGDEBUG, 1000, "({}) MSGQ_NOT_INITIALIZED", m_owner);
    return MSGQ_NOT_INITIALIZED;
  }
  if (!pMsg)
  {
    CLog::Log(LOGFATAL, "CDVDMessageQueue({})::Put MSGQ_INVALID_MSG", m_owner);
    return MSGQ_INVALID_MSG;
  }

  if (priority > 0)
  {
    int prio = priority;
    if (!front)
      prio++;

    auto it = std::find_if(m_prioMessages.begin(), m_prioMessages.end(),
                           [prio](const DVDMessageListItem &item){
                             return prio <= item.priority;
                           });
    m_prioMessages.emplace(it, pMsg, priority);
  }
  else
  {
    if (m_messages.empty())
    {
      m_iDataSize = 0;
      m_TimeBack = DVD_NOPTS_VALUE;
      m_TimeFront = DVD_NOPTS_VALUE;
    }

    if (front)
      m_messages.emplace_front(pMsg, priority);
    else
      m_messages.emplace_back(pMsg, priority);
  }

  m_msgqLogging = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                  CServiceBroker::GetLogging().CanLogComponent(LOGAVTIMING);

  if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET) && priority == 0)
  {
    DemuxPacket* packet = static_cast<CDVDMsgDemuxerPacket*>(pMsg.get())->GetPacket();
    if (packet)
    {
      m_iDataSize += packet->iSize;
      if (m_msgqLogging)
        m_msgqPuts++;
      if (front)
        UpdateTimeFront();
      else
        UpdateTimeBack();
    }
  }

  bool msgqEmit = false;
  double msgqFront = 0.0;
  double msgqBack = 0.0;
  double msgqBackLive = 0.0;
  double msgqDataMB = 0.0;
  double msgqElapsedMs = 0.0;
  size_t msgqMsgs = 0;
  size_t msgqPrio = 0;
  unsigned int msgqP = 0;
  unsigned int msgqG = 0;
  unsigned int msgqGP = 0;
  unsigned int msgqBNP = 0;
  const char* msgqBackState = "pkt";

  if (m_msgqLogging)
  {
    const auto msgqNow = std::chrono::steady_clock::now();
    if (m_msgqLogTime.time_since_epoch().count() == 0 || msgqNow - m_msgqLogTime >= 1000ms)
    {
      msgqElapsedMs =
          m_msgqLogTime.time_since_epoch().count() == 0
              ? 0.0
              : std::chrono::duration<double, std::milli>(msgqNow - m_msgqLogTime).count();
      m_msgqLogTime = msgqNow;

      msgqBackLive = DVD_NOPTS_VALUE;
      if (m_messages.empty())
        msgqBackState = "empty";
      else if (!m_messages.back().message->IsType(CDVDMsg::DEMUXER_PACKET))
        msgqBackState = "notpkt";
      else
      {
        DemuxPacket* backPacket =
            std::static_pointer_cast<CDVDMsgDemuxerPacket>(m_messages.back().message)->GetPacket();
        if (!backPacket)
          msgqBackState = "null";
        else if (backPacket->dts != DVD_NOPTS_VALUE)
          msgqBackLive = backPacket->dts;
        else if (backPacket->pts != DVD_NOPTS_VALUE)
          msgqBackLive = backPacket->pts;
        else
          msgqBackState = "nopts";
      }

      msgqEmit = true;
      msgqFront = m_TimeFront / DVD_TIME_BASE;
      msgqBack = m_TimeBack / DVD_TIME_BASE;
      msgqBackLive /= DVD_TIME_BASE;
      msgqDataMB = static_cast<double>(m_iDataSize) / 1048576.0;
      msgqMsgs = m_messages.size();
      msgqPrio = m_prioMessages.size();
      msgqP = m_msgqPuts;
      msgqG = m_msgqGets;
      msgqGP = m_msgqGetsPrio;
      msgqBNP = m_msgqBackNotPacket;
      m_msgqPuts = 0;
      m_msgqGets = 0;
      m_msgqGetsPrio = 0;
      m_msgqBackNotPacket = 0;
    }
  }

  // inform waiter for new packet
  lock.unlock();

  if (msgqEmit)
    logComponentM(LOGDEBUG, LOGAVTIMING,
                  "msgq({}) front={:.3f} back={:.3f} backLive={:.3f} backState={} span={:.2f} "
                  "msgs={} prio={} dataMB={:.1f} elapsedMs={:.0f} puts={} gets={} getsPrio={} "
                  "backNotPkt={}",
                  m_owner, msgqFront, msgqBack, msgqBackLive, msgqBackState, msgqFront - msgqBack,
                  msgqMsgs, msgqPrio, msgqDataMB, msgqElapsedMs, msgqP, msgqG, msgqGP, msgqBNP);
  m_hEvent.Set();

  return MSGQ_OK;
}

MsgQueueReturnCode CDVDMessageQueue::Get(std::shared_ptr<CDVDMsg>& pMsg,
                                         std::chrono::milliseconds timeout,
                                         int& priority)
{
  std::unique_lock lock(m_section);

  int ret = 0;

  if (!m_bInitialized)
  {
    CLog::Log(LOGFATAL, "CDVDMessageQueue({})::Get MSGQ_NOT_INITIALIZED", m_owner);
    return MSGQ_NOT_INITIALIZED;
  }

  while (!m_bAbortRequest)
  {
    std::list<DVDMessageListItem> &msgs = (priority > 0 || !m_prioMessages.empty()) ? m_prioMessages : m_messages;

    if (!msgs.empty() && (msgs.back().priority >= priority || m_drain))
    {
      DVDMessageListItem& item(msgs.back());
      priority = item.priority;

      if (item.message->IsType(CDVDMsg::DEMUXER_PACKET) && item.priority == 0)
      {
        DemuxPacket* packet =
            std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
        if (packet)
        {
          m_iDataSize -= packet->iSize;
        }
      }

      pMsg = std::move(item.message);
      msgs.pop_back();
      UpdateTimeBack();
      if (m_msgqLogging)
      {
        m_msgqGets++;
        if (&msgs == &m_prioMessages)
          m_msgqGetsPrio++;
      }
      ret = MSGQ_OK;
      break;
    }
    else if (timeout == 0ms)
    {
      ret = MSGQ_TIMEOUT;
      break;
    }
    else
    {
      m_hEvent.Reset();
      lock.unlock();

      // wait for a new message
      if (!m_hEvent.Wait(timeout))
        return MSGQ_TIMEOUT;

      lock.lock();
    }
  }

  if (m_bAbortRequest)
    return MSGQ_ABORT;

  return (MsgQueueReturnCode)ret;
}

void CDVDMessageQueue::UpdateTimeFront()
{
  if (!m_messages.empty())
  {
    auto &item = m_messages.front();
    if (item.message->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* packet =
          std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
      if (packet)
      {
        if (packet->dts != DVD_NOPTS_VALUE)
          m_TimeFront = packet->dts;
        else if (packet->pts != DVD_NOPTS_VALUE)
          m_TimeFront = packet->pts;

        if (m_TimeBack == DVD_NOPTS_VALUE)
          m_TimeBack = m_TimeFront;
      }
    }
  }
}

void CDVDMessageQueue::UpdateTimeBack()
{
  if (!m_messages.empty())
  {
    auto &item = m_messages.back();
    if (item.message->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* packet =
          std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
      if (packet)
      {
        if (packet->dts != DVD_NOPTS_VALUE)
          m_TimeBack = packet->dts;
        else if (packet->pts != DVD_NOPTS_VALUE)
          m_TimeBack = packet->pts;

        if (m_TimeFront == DVD_NOPTS_VALUE)
          m_TimeFront = m_TimeBack;
      }
    }
    else if (m_msgqLogging)
    {
      m_msgqBackNotPacket++;
    }
  }
}

unsigned CDVDMessageQueue::GetPacketCount(CDVDMsg::Message type) const {
  std::lock_guard lock(m_section);

  if (!m_bInitialized)
    return 0;

  unsigned count = 0;
  for (const auto &item : m_messages)
  {
    if(item.message->IsType(type))
      count++;
  }
  for (const auto &item : m_prioMessages)
  {
    if(item.message->IsType(type))
      count++;
  }

  return count;
}

void CDVDMessageQueue::WaitUntilEmpty()
{
  size_t remaining;
  {
    std::lock_guard lock(m_section);

    m_drain = true;
    remaining = m_messages.size() + m_prioMessages.size();
  }

  const double queuedSec = GetTimeSize();
  logComponentM(LOGDEBUG, LOGAVTIMING, "queue({}) drain start: {} msgs, {:.2f}s queued", m_owner,
                remaining, queuedSec);

  const auto ceiling = std::chrono::milliseconds(
      std::clamp(static_cast<int>(queuedSec * 1000.0) + 3000, 8000, 30000));
  XbmcThreads::EndTime<> totalTimer(ceiling);
  XbmcThreads::EndTime<> stallTimer(1500ms);
  size_t lastRemaining = std::numeric_limits<size_t>::max();
  bool drained = false;
  constexpr size_t MIN_WINDOW_PROGRESS = 10;
  while (!m_bAbortRequest && !totalTimer.IsTimePast())
  {
    {
      std::lock_guard lock(m_section);
      remaining = m_messages.size() + m_prioMessages.size();
    }

    if (remaining == 0)
    {
      drained = true;
      break;
    }

    if (remaining + MIN_WINDOW_PROGRESS <= lastRemaining)
    {
      lastRemaining = remaining;
      stallTimer.Set(1500ms);
    }
    else if (stallTimer.IsTimePast())
    {
      logComponentM(LOGDEBUG, LOGAVTIMING, "queue({}) drain stalled/trickling, {} msgs left",
                    m_owner, remaining);
      break;
    }

    std::this_thread::sleep_for(25ms);
  }

  if (drained && !m_bAbortRequest)
  {
    auto msg = std::make_shared<CDVDMsgGeneralSynchronize>(2s, SYNCSOURCE_ANY);
    Put(msg);
    msg->Wait(m_bAbortRequest, 0);
  }

  if (totalTimer.IsTimePast())
    logComponentM(LOGDEBUG, LOGAVTIMING, "queue({}) drain ceiling hit after {}ms", m_owner,
                  ceiling.count());

  {
    std::lock_guard lock(m_section);

    m_drain = false;
  }
}

int CDVDMessageQueue::GetDataSize() const
{
  std::lock_guard lock(m_section);
  return m_iDataSize;
}

int CDVDMessageQueue::GetLevel(bool data_level) const
{
  std::lock_guard lock(m_section);

  if (m_iDataSize > m_iMaxDataSize)
    return 100;
  if (m_iDataSize == 0)
    return 0;

  if (IsDataBasedLocked() || data_level)
  {
    return std::min((uint64_t)100, 100 * m_iDataSize / m_iMaxDataSize);
  }

  int level = std::min(100.0, ceil(100.0 * m_TimeSize * (m_TimeFront - m_TimeBack) / DVD_TIME_BASE ));

  // if we added lots of packets with NOPTS, make sure that the queue is not signalled empty
  if (level == 0 && m_iDataSize != 0)
  {
    CLog::Log(LOGDEBUG, "CDVDMessageQueue::GetLevel() - can't determine level");
    return 1;
  }

  return level;
}

void CDVDMessageQueue::GetLevels(int& level, int& dataLevel) const
{
  std::unique_lock lock(m_section);

  if (m_iDataSize > m_iMaxDataSize)
  {
    level = 100;
    dataLevel = 100;
    return;
  }
  if (m_iDataSize == 0)
  {
    level = 0;
    dataLevel = 0;
    return;
  }

  dataLevel = static_cast<int>(std::min((uint64_t)100, 100 * m_iDataSize / m_iMaxDataSize));

  if (IsDataBasedLocked())
  {
    level = dataLevel;
    return;
  }

  level = std::min(100.0, ceil(100.0 * m_TimeSize * (m_TimeFront - m_TimeBack) / DVD_TIME_BASE));

  if (level == 0 && m_iDataSize != 0)
    level = 1;
}

double CDVDMessageQueue::GetTimeSize() const
{
  std::lock_guard lock(m_section);

  if (IsDataBasedLocked())
    return 0.0;
  else
    return (m_TimeFront - m_TimeBack) / DVD_TIME_BASE;
}

bool CDVDMessageQueue::IsDataBased() const
{
  std::unique_lock lock(m_section);
  return IsDataBasedLocked();
}

bool CDVDMessageQueue::IsDataBasedLocked() const
{
  return (m_TimeBack == DVD_NOPTS_VALUE  ||
          m_TimeFront == DVD_NOPTS_VALUE ||
          m_TimeFront <= m_TimeBack);
}
