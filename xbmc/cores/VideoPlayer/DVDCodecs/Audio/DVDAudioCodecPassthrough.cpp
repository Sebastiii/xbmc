/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDAudioCodecPassthrough.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
constexpr unsigned int TRUEHD_BUF_SIZE = 61440;
constexpr unsigned int EAC3_BURST_BUF_SIZE = 24576 - 8;

// Internal sentinel for "no valid PTS" - we use -1.0 instead of DVD_NOPTS_VALUE
// because DVD_NOPTS_VALUE (0xFFF0000000000000) when cast to double becomes ~1.844e19
// which is the exact garbage value we see from the demuxer during seamless branching
constexpr double LOCAL_NOPTS = -1.0;

// Helper to check if a PTS value is valid
// Valid PTS must be >= 0 and <= 24 hours (way beyond any real content)
constexpr double MAX_REASONABLE_PTS = 86400000000.0; // 24 hours in DVD_TIME_BASE units

inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}

constexpr unsigned int EAC3_BLOCKS_PER_FRAME[4] = {1, 2, 3, 6};

class CEac3HeaderBitReader
{
public:
  CEac3HeaderBitReader(const uint8_t* data, unsigned int size) : m_data(data), m_bits(size * 8) {}

  unsigned int Read(unsigned int count)
  {
    unsigned int value = 0;
    while (count--)
    {
      if (m_pos >= m_bits)
      {
        m_exhausted = true;
        return 0;
      }
      value = (value << 1) | ((m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1);
      m_pos++;
    }
    return value;
  }

  void Skip(unsigned int count)
  {
    m_pos += count;
    if (m_pos > m_bits)
      m_exhausted = true;
  }

  bool Exhausted() const { return m_exhausted; }

private:
  const uint8_t* m_data;
  unsigned int m_bits;
  unsigned int m_pos{0};
  bool m_exhausted{false};
};

bool Eac3FrameStartsAccessUnit(const uint8_t* data, unsigned int size)
{
  if (!data || size < 8)
    return true;

  CEac3HeaderBitReader br(data, std::min(size, 128u));
  if (br.Read(16) != 0x0B77)
    return true;

  const unsigned int strmtyp = br.Read(2);
  const unsigned int substreamid = br.Read(3);
  if (strmtyp == 3)
    return true;
  if (strmtyp == 1 || substreamid != 0)
    return false;

  br.Skip(11);
  const unsigned int fscod = br.Read(2);
  unsigned int blocks = 6;
  if (fscod == 3)
    br.Skip(2);
  else
    blocks = EAC3_BLOCKS_PER_FRAME[br.Read(2)];

  if (blocks == 6 || strmtyp == 2)
    return true;

  const unsigned int acmod = br.Read(3);
  const unsigned int lfeon = br.Read(1);
  br.Skip(10);
  if (br.Read(1))
    br.Skip(8);
  if (acmod == 0)
  {
    br.Skip(5);
    if (br.Read(1))
      br.Skip(8);
  }

  if (br.Read(1))
  {
    if (acmod > 2)
      br.Skip(2);
    if ((acmod & 1) && acmod > 2)
      br.Skip(6);
    if (acmod & 4)
      br.Skip(6);
    if (lfeon && br.Read(1))
      br.Skip(5);

    for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
    {
      if (br.Read(1))
        br.Skip(6);
    }
    if (br.Read(1))
      br.Skip(6);

    const unsigned int mixdef = br.Read(2);
    if (mixdef == 1)
      br.Skip(5);
    else if (mixdef == 2)
      br.Skip(12);
    else if (mixdef == 3)
      br.Skip((br.Read(5) + 2) * 8);

    if (acmod < 2)
    {
      for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
      {
        if (br.Read(1))
          br.Skip(14);
      }
    }

    if (br.Read(1))
    {
      for (unsigned int b = 0; b < blocks; b++)
      {
        if (blocks == 1 || br.Read(1))
          br.Skip(5);
      }
    }
  }

  if (br.Read(1))
  {
    br.Skip(5);
    if (acmod == 2)
      br.Skip(4);
    if (acmod >= 6)
      br.Skip(2);
    for (unsigned int i = 0; i < (acmod ? 1u : 2u); i++)
    {
      if (br.Read(1))
        br.Skip(8);
    }
    if (fscod < 3)
      br.Skip(1);
  }

  const unsigned int convsync = br.Read(1);
  if (br.Exhausted())
    return true;

  return convsync == 1;
}
}

CDVDAudioCodecPassthrough::CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType) :
  CDVDAudioCodec(processInfo)
{
  m_format.m_streamInfo.m_type = streamType;
  m_deviceIsRAW = processInfo.WantsRawPassthrough();

  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
    {
      settings->RegisterCallback(this, {CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM,
                                        CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH});
    }
  }

  UpdateDialNormSettings();

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->UnregisterCallback(this);
  }

  Dispose();
}

void CDVDAudioCodecPassthrough::UpdateDialNormSettings()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings) return;

  m_defeatAC3DialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM));
  m_defeatEAC3AtmosDialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM));
  m_defeatTrueHDDialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM));
  m_defeatDTSDialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM));
}

void CDVDAudioCodecPassthrough::UpdateLavModeSettings()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings) return;

  const int algoValue = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
  const bool realtime = m_processInfo.IsRealtimeStream();
  const bool enableLavFull = !realtime && (algoValue == 3 || algoValue == 5);
  const bool enableLavSeamlessBranch =
      !realtime && (algoValue != 0 && algoValue != 3 && algoValue != 5);

  m_lavStyleSyncEnabled = enableLavFull;
  m_lavSeamlessBranchEnabled = enableLavSeamlessBranch;

  if (m_packerMAT)
    m_packerMAT->SetLavStyleEnabled(m_lavStyleSyncEnabled || m_lavSeamlessBranchEnabled);
}

void CDVDAudioCodecPassthrough::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting) return;

  const std::string& settingId = setting->GetId();
  if (settingId == CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM ||
      settingId == CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM ||
      settingId == CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM ||
      settingId == CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM)
  {
    UpdateDialNormSettings();
  }
  else if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH)
  {
    UpdateLavModeSettings();
  }
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  UpdateDialNormSettings();
  UpdateLavModeSettings();

  m_parser.SetCoreOnly(false);
  m_parser.SetDtsX(false);

  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_parser.SetSyncFamily(CAEStreamParser::SyncFamily::AC3);
      break;
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      m_parser.SetSyncFamily(CAEStreamParser::SyncFamily::DTS);
      break;
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_parser.SetSyncFamily(CAEStreamParser::SyncFamily::TrueHD);
      break;
    default:
      m_parser.SetSyncFamily(CAEStreamParser::SyncFamily::Any);
      break;
  }

  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_codecName = "pt-ac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm.load());
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_codecName = "pt-eac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_isEAC3JOC = (hints.profile == AV_PROFILE_EAC3_DDP_ATMOS);
      m_parser.SetEAC3JOC(m_isEAC3JOC);
      if (!m_isEAC3JOC || m_defeatEAC3AtmosDialNorm.load())
        m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm.load());
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      m_codecName = "pt-dtshd_ma";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_parser.SetDtsX(hints.profile == AV_PROFILE_DTS_HD_MA_X ||
                       hints.profile == AV_PROFILE_DTS_HD_MA_X_IMAX);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd_hra";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD;
      m_parser.SetDefeatTrueHDDialNorm(m_defeatTrueHDDialNorm.load());

      CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - passthrough output device is {}",
                __func__, m_deviceIsRAW ? "RAW" : "IEC");
      break;

    default:
      return false;
  }

  if (!m_lavStyleSyncEnabled && !m_lavSeamlessBranchEnabled)
  {
    logM(LOGDEBUG, "LAV all sync DISABLED, using standard Kodi PTS handling for {}", m_codecName);
  }

  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;
  
  if (m_lavStyleSyncEnabled)
  {
    // LAV Full: Use LOCAL_NOPTS sentinel for PTS validation
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_lastOutputPts = LOCAL_NOPTS;
    m_jitterTracker.Reset();
  }
  else
  {
    // Standard Kodi: Use DVD_NOPTS_VALUE
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
  return true;
}

void CDVDAudioCodecPassthrough::Dispose()
{
  if (m_buffer)
  {
    delete[] m_buffer;
    m_buffer = nullptr;
  }

  free(m_backlogBuffer);
  m_backlogBuffer = nullptr;
  m_backlogBufferSize = 0;

  m_bufferSize = 0;
}

bool CDVDAudioCodecPassthrough::AddData(const DemuxPacket &packet)
{
  // Apply cached values (updated by settings callbacks) without per-packet settings lookups.
  // Skip E-AC-3 dialnorm defeat for JOC/Atmos unless explicitly overridden —
  // modifying BSI dialnorm breaks JOC rendering on receivers.
  m_parser.SetDefeatAC3DialNorm(
    m_defeatAC3DialNorm.load() && (!m_isEAC3JOC || m_defeatEAC3AtmosDialNorm.load()));
  m_parser.SetDefeatTrueHDDialNorm(m_defeatTrueHDDialNorm.load());
  m_parser.SetDefeatDTSDialNorm(m_defeatDTSDialNorm.load());

  if (m_backlogSize)
  {
    m_dataSize = m_bufferSize;
    unsigned int consumed = m_parser.AddData(m_backlogBuffer, m_backlogSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    if (consumed != m_backlogSize)
    {
      memmove(m_backlogBuffer, m_backlogBuffer+consumed, m_backlogSize-consumed);
    }
    m_backlogSize -= consumed;
  }

  auto pData(const_cast<uint8_t*>(packet.pData));
  int iSize(packet.iSize);

  if (m_lavStyleSyncEnabled)
  {
    // LAV Full: Detect invalid PTS values using robust check for seamless branching
    double incomingPts = packet.pts;
    bool ptsIsValid = IsValidPts(incomingPts);

    if (pData)
    {
      // Sanitize PTS members if they contain garbage values (can happen during seamless branching)
      if (!IsValidPts(m_currentPts))
        m_currentPts = LOCAL_NOPTS;
      if (!IsValidPts(m_nextPts))
        m_nextPts = LOCAL_NOPTS;

      if (m_currentPts == LOCAL_NOPTS)
      {
        if (m_nextPts != LOCAL_NOPTS)
        {
          m_currentPts = m_nextPts;
          m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
        }
        else if (ptsIsValid)
        {
          m_currentPts = incomingPts;
        }
      }
      else if (ptsIsValid)
      {
        m_nextPts = incomingPts;
      }
    }
  }
  else
  {
    // Standard Kodi/avdvplus: Original PTS handling
    if (pData)
    {
      if (m_currentPts == DVD_NOPTS_VALUE)
      {
        if (m_nextPts != DVD_NOPTS_VALUE)
        {
          m_currentPts = m_nextPts;
          m_nextPts = packet.pts;
        }
        else if (packet.pts != DVD_NOPTS_VALUE)
        {
          m_currentPts = packet.pts;
        }
      }
      else
      {
        m_nextPts = packet.pts;
      }
    }
  }

  if (pData && !m_backlogSize && !m_dataSize)
  {
    if (iSize <= 0)
      return true;

    m_dataSize = m_bufferSize;
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);

    if (used != iSize)
    {
      const unsigned int remaining = static_cast<unsigned int>(iSize - used);
      if (m_backlogBufferSize < remaining)
      {
        m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, remaining);
        m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
      }
      m_backlogSize = remaining;
      memcpy(m_backlogBuffer, pData + used, m_backlogSize);
    }
  }
  else if (pData)
  {
    const unsigned int newSize = m_backlogSize + static_cast<unsigned int>(iSize);
    if (m_backlogBufferSize < newSize)
    {
      m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, newSize);
      m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
    }
    memcpy(m_backlogBuffer + m_backlogSize, pData, iSize);
    m_backlogSize += static_cast<unsigned int>(iSize);
  }

  if (!m_dataSize)
    return true;

  const CAEStreamInfo& parsedInfo = m_parser.GetStreamInfo();
  const unsigned int parsedRate = m_parser.GetSampleRate();
  const unsigned int parsedChannels = m_parser.GetChannels();
  if (m_format.m_dataFormat != AE_FMT_RAW || !(m_format.m_streamInfo == parsedInfo) ||
      m_format.m_sampleRate != parsedRate ||
      m_format.m_channelLayout.Count() != parsedChannels ||
      m_format.m_streamInfo.m_atmosObjects != parsedInfo.m_atmosObjects ||
      m_format.m_streamInfo.m_atmosChannels != parsedInfo.m_atmosChannels ||
      m_format.m_streamInfo.m_bedChannels != parsedInfo.m_bedChannels ||
      m_format.m_streamInfo.m_bedIsLfeOnly != parsedInfo.m_bedIsLfeOnly)
  {
    m_format.m_dataFormat = AE_FMT_RAW;
    m_format.m_streamInfo = parsedInfo;
    m_format.m_sampleRate = parsedRate;
    m_format.m_frameSize = 1;
    CAEChannelInfo layout;
    for (unsigned int i = 0; i < parsedChannels; i++)
    {
      layout += AE_CH_RAW;
    }
    m_format.m_channelLayout = layout;
  }

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (m_deviceIsRAW) // RAW
    {
      m_dataSize = PackTrueHD();
    }
    else // IEC
    {
      if (m_lavStyleSyncEnabled)
      {
        // LAV Full: timestamp caching for TrueHD MAT assembly
        // Since a MAT frame contains 24 TrueHD frames, we want the timestamp of the first one
        if (!m_truehd_ptsCacheValid && IsValidPts(m_currentPts))
        {
          m_truehd_ptsCache = m_currentPts;
          m_truehd_ptsCacheValid = true;
        }
      }

      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;

        if (m_lavStyleSyncEnabled)
        {
          // Consume discontinuity flag from MAT packer (seamless branch detection)
          // We don't need to react to it - LAV packer already handled padding
          // and our internal clock continues smoothly regardless
          (void)m_packerMAT->HadDiscontinuity();

          // Use cached timestamp for this MAT frame, then reset cache for next MAT
          if (m_truehd_ptsCacheValid)
          {
            m_currentPts = m_truehd_ptsCache;
            m_truehd_ptsCacheValid = false;
            m_truehd_ptsCache = LOCAL_NOPTS;
          }
        }
      }
      else
      {
        m_dataSize = 0;
      }
    }
  }
  else if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 &&
           m_format.m_streamInfo.m_repeat > 1)
  {
    m_dataSize = PackEAC3();

    if (m_dataSize)
    {
      if (m_lavStyleSyncEnabled)
        m_nextPts = LOCAL_NOPTS;
      else
        m_nextPts = DVD_NOPTS_VALUE;
    }
  }

  return true;
}

unsigned int CDVDAudioCodecPassthrough::PackEAC3()
{
  const unsigned int framesPerBurst = m_format.m_streamInfo.m_repeat;

  if (m_eac3FramesPerBurst != framesPerBurst)
  {
    m_eac3Size = 0;
    m_eac3FramesCount = 0;
    m_eac3FramesPerBurst = framesPerBurst;
    m_eac3AlignDiscards = 0;
    m_eac3AlignGiveUp = false;
  }

  if (framesPerBurst <= 1)
    return m_dataSize;

  if (m_eac3FramesCount == 0 && !m_eac3AlignGiveUp)
  {
    if (Eac3FrameStartsAccessUnit(m_buffer, m_dataSize))
    {
      if (m_eac3AlignDiscards > 0)
        logM(LOGINFO, "E-AC3 burst aligned to access-unit start after discarding {} frames",
             m_eac3AlignDiscards);
      m_eac3AlignDiscards = 0;
    }
    else if (m_eac3AlignDiscards + 1 >= framesPerBurst)
    {
      logM(LOGWARNING, "no E-AC3 access-unit start within {} frames, disabling burst alignment",
           framesPerBurst);
      m_eac3AlignGiveUp = true;
      m_eac3AlignDiscards = 0;
    }
    else
    {
      m_eac3AlignDiscards++;
      m_currentPts = m_nextPts;
      m_nextPts = m_lavStyleSyncEnabled ? LOCAL_NOPTS : DVD_NOPTS_VALUE;
      return 0;
    }
  }

  if (m_eac3Buffer.empty())
    m_eac3Buffer.resize(EAC3_BURST_BUF_SIZE);

  const unsigned int newSize = m_eac3Size + m_dataSize;
  const bool overrun = newSize > EAC3_BURST_BUF_SIZE;

  if (!overrun)
  {
    memcpy(m_eac3Buffer.data() + m_eac3Size, m_buffer, m_dataSize);
    m_eac3Size = newSize;
    m_eac3FramesCount++;
  }

  if (m_eac3FramesCount >= m_eac3FramesPerBurst || overrun)
  {
    const unsigned int burstSize = m_eac3Size;
    m_eac3Size = 0;
    m_eac3FramesCount = 0;
    return burstSize;
  }

  return 0;
}

unsigned int CDVDAudioCodecPassthrough::PackTrueHD()
{
  unsigned int dataSize{0};

  if (m_trueHDoffset == 0)
    m_trueHDframes = 0;

  memcpy(m_trueHDBuffer.data() + m_trueHDoffset, m_buffer, m_dataSize);

  m_trueHDoffset += m_dataSize;
  m_trueHDframes++;

  if (m_trueHDframes == 24)
  {
    dataSize = m_trueHDoffset;
    m_trueHDoffset = 0;
    m_trueHDframes = 0;
    return dataSize;
  }

  return 0;
}

void CDVDAudioCodecPassthrough::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = GetData(frame.data);
  frame.framesOut = 0;
  frame.hasDiscontinuity = false;
  frame.discontinuityCorrection = 0.0;

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = 8;
  frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());

  if (m_lavStyleSyncEnabled)
  {
    //============================================================================
    // LAV Internal Clock A/V Sync
    //============================================================================
    // Based on LAV Filters by Hendrik Leppkes (Nevcairiel)
    // 
    // We maintain our OWN internal clock (m_internalClock) that:
    // - Syncs to RESYNC PTS from VideoPlayer (coordinated A/V clock)
    // - Outputs PTS from our clock, not demuxer
    // - Tracks drift against demuxer to detect discontinuities
    //============================================================================

    const CAEStreamInfo::DataType streamType = m_format.m_streamInfo.m_type;
    const bool isTrueHD = (streamType == CAEStreamInfo::STREAM_TYPE_TRUEHD);

    // TrueHD-specific: Get samples offset for drift calculation (LAV)
    double samplesOffsetTime = 0.0;
    if (isTrueHD && m_packerMAT && m_format.m_sampleRate > 0)
    {
      int samplesOffset = m_packerMAT->GetSamplesOffset();
      if (samplesOffset != 0)
      {
        samplesOffsetTime = static_cast<double>(samplesOffset) / m_format.m_sampleRate * DVD_TIME_BASE;
      }
    }

    // Demuxer PTS for this frame (may be invalid during branching)
    const double demuxerPts = m_currentPts;
    const bool haveDemuxerPts = IsValidPts(demuxerPts);

    //============================================================================
    // STEP 1: Resync internal clock if needed
    //============================================================================
    // Sync to demuxer PTS when we need resync and have valid PTS.
    // This happens on codec creation and after seeks.
    // 
    // If RESYNC arrives later (from VideoPlayer::Sync), SyncToResyncPts() will
    // override this with the correct coordinated A/V clock value.
    // This approach handles display reset codec recreation gracefully - the new
    // codec syncs to demuxer PTS, which should be close to correct since the
    // stream is already playing.
    if (m_needsResync && haveDemuxerPts)
    {
      m_internalClock = demuxerPts;
      m_needsResync = false;
      m_jitterTracker.Reset();  // Clear jitter history on resync
      
      CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough: Internal clock synced to demuxer PTS {:.3f}s",
                demuxerPts / DVD_TIME_BASE);
    }

    //============================================================================
    // STEP 2: Track jitter and correct internal clock when threshold exceeded
    //============================================================================
    // LAV Filters approach: track drift between our internal clock and demuxer PTS.
    // When drift exceeds threshold, CORRECT the internal clock to realign.
    // This handles both:
    // - Seamless branch points (large sudden jumps in demuxer PTS)
    // - Long-term drift accumulation
    //============================================================================
    
    if (IsValidPts(m_internalClock) && haveDemuxerPts)
    {
      // Jitter = our_clock - demuxer_pts (+ samplesOffset for TrueHD MAT compensation)
      // Positive jitter = we're ahead of demuxer, negative = we're behind
      double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
      m_jitterTracker.Sample(jitter);

      // Use AbsMinimum for correction (most stable value in the window)
      double absMinJitter = m_jitterTracker.AbsMinimum();

      if (std::abs(absMinJitter) > m_jitterThreshold)
      {
        // Correct internal clock by the jitter amount (like LAV Filters)
        m_internalClock -= absMinJitter;
        m_jitterTracker.OffsetValues(-absMinJitter);
        
        // Signal discontinuity to downstream
        frame.hasDiscontinuity = true;
        frame.discontinuityCorrection = absMinJitter;
        
        logComponentM(LOGDEBUG, LOGAUDIO, "Jitter correction {:.2f}ms (threshold {:.0f}ms)",
                      absMinJitter / 1000.0, m_jitterThreshold / 1000.0);
      }

      if (++m_jitterTraceCount >= 100)
      {
        m_jitterTraceCount = 0;
        logComponentM(LOGDEBUG, LOGAUDIO,
                      "standing jitter {:+.2f}ms (absmin {:+.2f}ms, threshold {:.0f}ms)",
                      jitter / 1000.0, absMinJitter / 1000.0, m_jitterThreshold / 1000.0);
      }
    }

    //============================================================================
    // STEP 3: Output PTS from internal clock (synced to RESYNC)
    //============================================================================
    // The whole point of LAV sync is to output PTS from our internal clock
    // which has been synced to the RESYNC pts (coordinated A/V clock).
    // 
    // The internal clock:
    // - Is synced to RESYNC pts (from VideoPlayer::Sync, the authoritative A/V clock)
    // - Advances by frame duration each frame
    // - Tracks drift against demuxer PTS for discontinuity detection
    //
    // We ALWAYS use internal clock for output after it's been synced.
    // The demuxer PTS is only used for:
    // - Initial sync before RESYNC arrives
    // - Drift tracking (to detect discontinuities)
    //============================================================================
    
    if (IsValidPts(m_internalClock))
    {
      // Output from internal clock (synced to RESYNC)
      frame.pts = m_internalClock;
      m_internalClock += frame.duration;
      m_lastOutputPts = frame.pts;
    }
    else if (haveDemuxerPts)
    {
      // Fallback: internal clock not yet set, use demuxer PTS
      // This happens before RESYNC arrives
      frame.pts = demuxerPts;
      m_internalClock = demuxerPts + frame.duration;
      m_lastOutputPts = frame.pts;
    }
    else
    {
      // No valid PTS available anywhere
      frame.pts = DVD_NOPTS_VALUE;
    }

    // Clear current PTS after use
    m_currentPts = LOCAL_NOPTS;
  }
  else
  {
    //============================================================================
    // Standard Kodi PTS handling (no LAV sync)
    //============================================================================
    // Original avdvplus code
    //============================================================================

    frame.pts = m_currentPts;
    m_currentPts = DVD_NOPTS_VALUE;
  }
}

int CDVDAudioCodecPassthrough::GetData(uint8_t** dst)
{
  if (!m_dataSize)
    AddData(DemuxPacket());

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    *dst = m_trueHDBuffer.data();
  else if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 &&
           m_eac3FramesPerBurst > 1)
    *dst = m_eac3Buffer.data();
  else
    *dst = m_buffer;

  int bytes = m_dataSize;
  m_dataSize = 0;
  return bytes;
}

void CDVDAudioCodecPassthrough::Reset()
{
  m_trueHDoffset = 0;
  m_eac3Size = 0;
  m_eac3FramesCount = 0;
  m_eac3FramesPerBurst = 0;
  m_eac3AlignDiscards = 0;
  m_eac3AlignGiveUp = false;
  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;

  if (m_lavStyleSyncEnabled)
  {
    // LAV Full reset: use LOCAL_NOPTS sentinel
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_lastOutputPts = LOCAL_NOPTS;

    // Reset TrueHD-specific state
    m_truehd_ptsCache = LOCAL_NOPTS;
    m_truehd_ptsCacheValid = false;

    // Reset LAV internal clock - will resync on next valid PTS or RESYNC
    m_internalClock = LOCAL_NOPTS;
    m_needsResync = true;
    m_jitterTracker.Reset();

    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::Reset - Internal clock reset, will resync");

    // Reset PackerMAT state for TrueHD
    if (m_packerMAT)
      m_packerMAT->SoftReset();
    
    m_parser.Reset();
  }
  else
  {
    // Standard Kodi/avdvplus reset - EXACT order from original
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
    if (m_packerMAT)
      m_packerMAT->SoftReset();
    m_parser.Reset();
  }
}

void CDVDAudioCodecPassthrough::SetLavStyleSyncEnabled(bool enabled)
{
  m_lavStyleSyncEnabled = enabled;

  // LAV Full also enables seamless branch fix
  if (enabled)
    m_lavSeamlessBranchEnabled = true;

  // Propagate to PackerMAT for TrueHD discontinuity detection
  // PackerMAT should be enabled if EITHER full LAV sync OR seamless branch is enabled
  if (m_packerMAT)
    m_packerMAT->SetLavStyleEnabled(m_lavStyleSyncEnabled || m_lavSeamlessBranchEnabled);
}

void CDVDAudioCodecPassthrough::SetLavSeamlessBranchEnabled(bool enabled)
{
  m_lavSeamlessBranchEnabled = enabled;

  // Propagate to PackerMAT for TrueHD discontinuity detection
  // PackerMAT should be enabled if EITHER full LAV sync OR seamless branch is enabled
  if (m_packerMAT)
    m_packerMAT->SetLavStyleEnabled(m_lavStyleSyncEnabled || m_lavSeamlessBranchEnabled);
}

void CDVDAudioCodecPassthrough::ResetLavSyncState()
{
  if (!m_lavStyleSyncEnabled)
    return;

  // Reset PTS tracking to force resync on next valid timestamp
  m_lastOutputPts = LOCAL_NOPTS;

  // Reset TrueHD timestamp cache
  m_truehd_ptsCache = LOCAL_NOPTS;
  m_truehd_ptsCacheValid = false;

  // Reset internal clock - will resync to demuxer on next valid PTS
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();

  CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::ResetLavSyncState - Internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  if (!m_lavStyleSyncEnabled)
    return;

  // VideoPlayer::Sync() sends RESYNC with a coordinated A/V clock value.
  // We trust this value and use it directly for our internal clock.
  // VideoPlayer.cpp has been modified to only send RESYNC when both
  // audio AND video have valid PTS values.
  
  if (pts != DVD_NOPTS_VALUE && pts >= 0.0 && pts <= MAX_REASONABLE_PTS)
  {
    m_internalClock = pts;
    m_jitterTracker.Reset();

    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Internal clock provisionally set to RESYNC pts {:.3f}s (awaiting demuxer)",
              pts / DVD_TIME_BASE);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Invalid pts, ignoring");
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}

