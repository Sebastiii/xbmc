/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guilib/guiinfo/PlayerGUIInfo.h"

#include "FileItem.h"
#include "PlayListPlayer.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "application/ApplicationVolumeHandling.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Video/AMLFrameMetadata.h"
#include "cores/EdlEdit.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIDialog.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/guiinfo/GUIInfo.h"
#include "guilib/guiinfo/GUIInfoHelper.h"
#include "guilib/guiinfo/GUIInfoLabels.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "utils/AMLUtils.h"
#include "utils/BitstreamConverter.h"
#include "utils/TimeUtils.h"

#include "platform/linux/SysfsPath.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <unordered_map>
#include <utility>
#include <memory>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

extern "C"
{
#include <libavutil/pixdesc.h>
}

using namespace KODI::GUILIB::GUIINFO;

namespace
{
constexpr unsigned int AML_CONFIG_THROTTLE_MS = 500;

bool SameDoViStreamInfo(const DOVIStreamInfo& a, const DOVIStreamInfo& b)
{
  return a.dovi_el_type == b.dovi_el_type && a.has_config == b.has_config &&
         a.has_header == b.has_header && a.is_dual_track == b.is_dual_track &&
         a.dovi.dv_version_major == b.dovi.dv_version_major &&
         a.dovi.dv_version_minor == b.dovi.dv_version_minor &&
         a.dovi.dv_profile == b.dovi.dv_profile && a.dovi.dv_level == b.dovi.dv_level &&
         a.dovi.rpu_present_flag == b.dovi.rpu_present_flag &&
         a.dovi.el_present_flag == b.dovi.el_present_flag &&
         a.dovi.bl_present_flag == b.dovi.bl_present_flag &&
         a.dovi.dv_bl_signal_compatibility_id == b.dovi.dv_bl_signal_compatibility_id;
}

bool SameDoViStreamMeta(const DOVIStreamMetadata& a, const DOVIStreamMetadata& b)
{
  return a.source_min_pq == b.source_min_pq && a.source_max_pq == b.source_max_pq &&
         a.has_level6_metadata == b.has_level6_metadata && a.level6_max_lum == b.level6_max_lum &&
         a.level6_min_lum == b.level6_min_lum && a.level6_max_cll == b.level6_max_cll &&
         a.level6_max_fall == b.level6_max_fall && a.meta_version == b.meta_version;
}

bool SameHdrStatic(const HDRStaticMetadataInfo& a, const HDRStaticMetadataInfo& b)
{
  return a.has_mdcv_metadata == b.has_mdcv_metadata && a.max_lum == b.max_lum &&
         a.min_lum == b.min_lum && a.colour_primaries == b.colour_primaries &&
         a.has_cll_metadata == b.has_cll_metadata && a.max_cll == b.max_cll &&
         a.max_fall == b.max_fall;
}

bool IsDvStreamCacheableLabel(int info)
{
  switch (info)
  {
    case PLAYER_PROCESS_VIDEO_DOVI_HAS_CONFIG:
    case PLAYER_PROCESS_VIDEO_DOVI_VERSION_MAJOR:
    case PLAYER_PROCESS_VIDEO_DOVI_VERSION_MINOR:
    case PLAYER_PROCESS_VIDEO_DOVI_PROFILE:
    case PLAYER_PROCESS_VIDEO_DOVI_LEVEL:
    case PLAYER_PROCESS_VIDEO_DOVI_RPU_PRESENT:
    case PLAYER_PROCESS_VIDEO_DOVI_EL_PRESENT:
    case PLAYER_PROCESS_VIDEO_DOVI_BL_PRESENT:
    case PLAYER_PROCESS_VIDEO_DOVI_BL_SIGNAL_COMPATIBILITY:
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_PROFILE:
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_BL_SIGNAL_COMPATIBILITY:
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_EL_PRESENT:
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_EL_TYPE:
    case PLAYER_PROCESS_VIDEO_DOVI_CODEC_FOURCC:
    case PLAYER_PROCESS_VIDEO_DOVI_CODEC_STRING:
    case PLAYER_PROCESS_VIDEO_DOVI_EL_TYPE:
    case PLAYER_PROCESS_VIDEO_DOVI_META_VERSION:
    case PLAYER_PROCESS_VIDEO_DOVI_HAS_HEADER:
    case PLAYER_PROCESS_VIDEO_DOVI_DUAL_TRACK:
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MIN_PQ:
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MAX_PQ:
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MIN_NITS:
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MAX_NITS:
    case PLAYER_PROCESS_VIDEO_DOVI_HAS_L6:
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_CLL:
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_FALL:
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MIN_LUM:
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_LUM:
    case PLAYER_PROCESS_VIDEO_HDR_HAS_CLL:
    case PLAYER_PROCESS_VIDEO_HDR_MAX_CLL:
    case PLAYER_PROCESS_VIDEO_HDR_MAX_FALL:
    case PLAYER_PROCESS_VIDEO_HDR_HAS_MDCV:
    case PLAYER_PROCESS_VIDEO_HDR_MIN_LUM:
    case PLAYER_PROCESS_VIDEO_HDR_MAX_LUM:
    case PLAYER_PROCESS_VIDEO_HDR_COLOUR_PRIMARIES:
      return true;
    default:
      return false;
  }
}
}

struct CPlayerGUIInfoFrameCache
{
  enum : uint32_t
  {
    DOVI_FRAME = 1u << 0,
    DOVI_STREAM = 1u << 1,
    DOVI_STREAM_META = 1u << 2,
    SRC_DOVI_STREAM = 1u << 3,
    HDR_STATIC = 1u << 4,
    DOVI_FOURCC = 1u << 5,
  };

  unsigned int token{~0u};
  uint32_t valid{0};
  DOVIFrameMetadata doViFrame;
  DOVIStreamInfo doViStream;
  DOVIStreamMetadata doViStreamMeta;
  DOVIStreamInfo srcDoViStream;
  HDRStaticMetadataInfo hdrStatic;
  std::string doviFourCC;
  std::string amlConfig;
  std::vector<std::string> amlConfigLines;
  std::string eotfValue;
  int fracRate{0};
  unsigned int amlReadMs{0};
  bool amlReadValid{false};
  unsigned int dvMode{0};
  unsigned int vpMode{0};

  void Tick()
  {
    const unsigned int t = CTimeUtils::GetFrameTime();
    if (t != token)
    {
      token = t;
      valid = 0;
    }
  }
  const DOVIFrameMetadata& DoViFrame()
  {
    Tick();
    if (!(valid & DOVI_FRAME))
    {
      doViFrame = CServiceBroker::GetDataCacheCore().GetVideoDoViFrameMetadata();
      valid |= DOVI_FRAME;
    }
    return doViFrame;
  }
  const DOVIStreamInfo& DoViStreamInfo()
  {
    Tick();
    if (!(valid & DOVI_STREAM))
    {
      doViStream = CServiceBroker::GetDataCacheCore().GetVideoDoViStreamInfo();
      valid |= DOVI_STREAM;
    }
    return doViStream;
  }
  const DOVIStreamMetadata& DoViStreamMeta()
  {
    Tick();
    if (!(valid & DOVI_STREAM_META))
    {
      doViStreamMeta = CServiceBroker::GetDataCacheCore().GetVideoDoViStreamMetadata();
      valid |= DOVI_STREAM_META;
    }
    return doViStreamMeta;
  }
  const DOVIStreamInfo& SourceDoViStreamInfo()
  {
    Tick();
    if (!(valid & SRC_DOVI_STREAM))
    {
      srcDoViStream = CServiceBroker::GetDataCacheCore().GetVideoSourceDoViStreamInfo();
      valid |= SRC_DOVI_STREAM;
    }
    return srcDoViStream;
  }
  const HDRStaticMetadataInfo& HdrStatic()
  {
    Tick();
    if (!(valid & HDR_STATIC))
    {
      hdrStatic = CServiceBroker::GetDataCacheCore().GetVideoHDRStaticMetadataInfo();
      valid |= HDR_STATIC;
    }
    return hdrStatic;
  }
  const std::string& DoViCodecFourCC()
  {
    Tick();
    if (!(valid & DOVI_FOURCC))
    {
      doviFourCC = CServiceBroker::GetDataCacheCore().GetVideoDoViCodecFourCC();
      valid |= DOVI_FOURCC;
    }
    return doviFourCC;
  }
  void RefreshDisplayState()
  {
    const unsigned int now = CTimeUtils::GetFrameTime();
    if (!amlReadValid || (now - amlReadMs) >= AML_CONFIG_THROTTLE_MS)
    {
      CSysfsPath config{"/sys/class/amhdmitx/amhdmitx0/config"};
      amlConfig = config.Exists() ? config.Get<std::string>().value_or("") : std::string();
      amlConfigLines = StringUtils::Split(amlConfig, "\n");
      eotfValue.clear();
      for (const std::string& line : amlConfigLines)
      {
        if (StringUtils::StartsWith(line, "EOTF: "))
        {
          eotfValue = line.substr(6);
          break;
        }
      }
      CSysfsPath fracRatePolicy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
      fracRate = fracRatePolicy.Exists() ? fracRatePolicy.Get<int>().value_or(0) : 0;
      dvMode = aml_dv_dolby_vision_mode();
      vpMode = aml_dv_video_processor_mode();
      amlReadMs = now;
      amlReadValid = true;
    }
  }
  const std::string& AmlConfig()
  {
    RefreshDisplayState();
    return amlConfig;
  }
  const std::vector<std::string>& AmlConfigLines()
  {
    RefreshDisplayState();
    return amlConfigLines;
  }
  int FracRatePolicy()
  {
    RefreshDisplayState();
    return fracRate;
  }
  unsigned int VpMode()
  {
    RefreshDisplayState();
    return vpMode;
  }
  unsigned int DvMode()
  {
    RefreshDisplayState();
    return dvMode;
  }
  const std::string& Eotf()
  {
    RefreshDisplayState();
    return eotfValue;
  }

  unsigned int dvGenToken{~0u};
  uint32_t dvStreamGen{0};
  bool dvSnapValid{false};
  DOVIStreamInfo dvSnapStreamInfo;
  DOVIStreamInfo dvSnapSrcStreamInfo;
  DOVIStreamMetadata dvSnapStreamMeta;
  HDRStaticMetadataInfo dvSnapHdrStatic;
  std::string dvSnapFourCC;
  std::unordered_map<int, std::pair<uint32_t, std::string>> labelResult;

  uint32_t DvStreamGen()
  {
    const unsigned int t = CTimeUtils::GetFrameTime();
    if (t != dvGenToken)
    {
      dvGenToken = t;
      const DOVIStreamInfo& si = DoViStreamInfo();
      const DOVIStreamInfo& ssi = SourceDoViStreamInfo();
      const DOVIStreamMetadata& sm = DoViStreamMeta();
      const HDRStaticMetadataInfo& hs = HdrStatic();
      const std::string& fc = DoViCodecFourCC();
      if (!dvSnapValid || !SameDoViStreamInfo(si, dvSnapStreamInfo) ||
          !SameDoViStreamInfo(ssi, dvSnapSrcStreamInfo) ||
          !SameDoViStreamMeta(sm, dvSnapStreamMeta) || !SameHdrStatic(hs, dvSnapHdrStatic) ||
          fc != dvSnapFourCC)
      {
        dvStreamGen++;
        dvSnapStreamInfo = si;
        dvSnapSrcStreamInfo = ssi;
        dvSnapStreamMeta = sm;
        dvSnapHdrStatic = hs;
        dvSnapFourCC = fc;
        dvSnapValid = true;
      }
    }
    return dvStreamGen;
  }

  bool TryGetLabel(int id, uint32_t gen, std::string& out)
  {
    const auto it = labelResult.find(id);
    if (it != labelResult.end() && it->second.first == gen)
    {
      out = it->second.second;
      return true;
    }
    return false;
  }

  void PutLabel(int id, uint32_t gen, const std::string& v) { labelResult[id] = {gen, v}; }
};

CPlayerGUIInfo::CPlayerGUIInfo()
  : m_appPlayer(CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>()),
    m_appVolume(CServiceBroker::GetAppComponents().GetComponent<CApplicationVolumeHandling>())
{
  m_frameCache = std::make_unique<CPlayerGUIInfoFrameCache>();
}

CPlayerGUIInfo::~CPlayerGUIInfo() = default;

int CPlayerGUIInfo::GetTotalPlayTime() const
{
  return std::lrint(g_application.GetTotalTime());
}

std::string CPlayerGUIInfo::GetAMLConfigInfo(std::string item) const
{
  std::string item_value = "unknown";
  std::vector<std::string> aml_config_item;
  std::vector<std::string>::const_iterator i;

  const std::vector<std::string>& aml_config_lines = m_frameCache->AmlConfigLines();
  for (i = aml_config_lines.begin(); i < aml_config_lines.end(); i++)
  {
    if (StringUtils::StartsWithNoCase(*i, item))
    {
      aml_config_item = StringUtils::Split(*i, ": ");
      if (aml_config_item.size() > 1)
      {
        if (StringUtils::EqualsNoCase(item, "VIC"))
        {
          std::vector<std::string> sub_items = StringUtils::Split(aml_config_item.at(1), " ");

          if (sub_items.size() > 1)
          {
            int cur_fractional_rate = m_frameCache->FracRatePolicy();
            item_value = StringUtils::Left(sub_items.at(1), sub_items.at(1).length() - 4) + " ";

            if (cur_fractional_rate)
            {
              float refreshrate = static_cast<float>(atof(StringUtils::Mid(sub_items.at(1), sub_items.at(1).length() - 4, 2).c_str()));
              item_value += fmt::format("{:.3f}", refreshrate / 1.001f) + "Hz";
            }
            else
              item_value += StringUtils::Mid(sub_items.at(1), sub_items.at(1).length() - 4, 2) + "Hz";
          }
        }
        else
          item_value = aml_config_item.at(1);
        break;
      }
    }
  }

  return item_value;
}

std::string CPlayerGUIInfo::GetHdr10LimitedValue(int source, int limit, int dvLevel6) const
{
  if (limit <= 0)
    return "";

  const std::string& eotf = m_frameCache->Eotf();
  if (!StringUtils::EqualsNoCase(eotf, "HDR10") && !StringUtils::EqualsNoCase(eotf, "HDR10+"))
    return "";

  if (dvLevel6 != 0)
    source = dvLevel6;

  if (source <= limit)
    return "";

  return std::to_string(limit);
}

int CPlayerGUIInfo::GetPlayTime() const
{
  return std::lrint(g_application.GetTime());
}

int CPlayerGUIInfo::GetPlayTimeRemaining() const
{
  int iReverse = GetTotalPlayTime() - std::lrint(g_application.GetTime());
  return iReverse > 0 ? iReverse : 0;
}

float CPlayerGUIInfo::GetSeekPercent() const
{
  int iTotal = GetTotalPlayTime();
  if (iTotal == 0)
    return 0.0f;

  float fPercentPlayTime = static_cast<float>(GetPlayTime() * 1000) / iTotal * 0.1f;
  float fPercentPerSecond = 100.0f / static_cast<float>(iTotal);
  float fPercent =
      fPercentPlayTime + fPercentPerSecond * m_appPlayer->GetSeekHandler().GetSeekSize();
  fPercent = std::max(0.0f, std::min(fPercent, 100.0f));
  return fPercent;
}

std::string CPlayerGUIInfo::GetCurrentPlayTime(TIME_FORMAT format) const
{
  if (format == TIME_FORMAT_GUESS && GetTotalPlayTime() >= 3600)
    format = TIME_FORMAT_HH_MM_SS;

  return StringUtils::SecondsToTimeString(std::lrint(GetPlayTime()), format);
}

std::string CPlayerGUIInfo::GetCurrentPlayTimeRemaining(TIME_FORMAT format) const
{
  if (format == TIME_FORMAT_GUESS && GetTotalPlayTime() >= 3600)
    format = TIME_FORMAT_HH_MM_SS;

  int iTimeRemaining = GetPlayTimeRemaining();
  if (iTimeRemaining)
    return StringUtils::SecondsToTimeString(iTimeRemaining, format);

  return std::string();
}

std::string CPlayerGUIInfo::GetDuration(TIME_FORMAT format) const
{
  int iTotal = GetTotalPlayTime();
  if (iTotal > 0)
  {
    if (format == TIME_FORMAT_GUESS && iTotal >= 3600)
      format = TIME_FORMAT_HH_MM_SS;
    return StringUtils::SecondsToTimeString(iTotal, format);
  }
  return std::string();
}

std::string CPlayerGUIInfo::GetCurrentSeekTime(TIME_FORMAT format) const
{
  if (format == TIME_FORMAT_GUESS && GetTotalPlayTime() >= 3600)
    format = TIME_FORMAT_HH_MM_SS;

  return StringUtils::SecondsToTimeString(
      g_application.GetTime() + m_appPlayer->GetSeekHandler().GetSeekSize(), format);
}

std::string CPlayerGUIInfo::GetSeekTime(TIME_FORMAT format) const
{
  if (!m_appPlayer->GetSeekHandler().HasTimeCode())
    return std::string();

  int iSeekTimeCode = m_appPlayer->GetSeekHandler().GetTimeCodeSeconds();
  if (format == TIME_FORMAT_GUESS && iSeekTimeCode >= 3600)
    format = TIME_FORMAT_HH_MM_SS;

  return StringUtils::SecondsToTimeString(iSeekTimeCode, format);
}

void CPlayerGUIInfo::SetShowInfo(bool showinfo)
{
  if (showinfo != m_playerShowInfo)
  {
    m_playerShowInfo = showinfo;
    m_events.Publish(PlayerShowInfoChangedEvent(m_playerShowInfo));
  }
}

bool CPlayerGUIInfo::ToggleShowInfo()
{
  SetShowInfo(!m_playerShowInfo);
  return m_playerShowInfo;
}

bool CPlayerGUIInfo::InitCurrentItem(CFileItem *item)
{
  if (item && m_appPlayer->IsPlaying())
  {
    CLog::Log(LOGDEBUG, "CPlayerGUIInfo::InitCurrentItem({})", CURL::GetRedacted(item->GetPath()));
    m_currentItem = std::make_unique<CFileItem>(*item);
  }
  else
  {
    m_currentItem.reset();
  }
  return false;
}

std::string HdrTypeToString(StreamHdrType hdrType) {
  switch (hdrType) {
    case StreamHdrType::HDR_TYPE_NONE: return "SDR";
    case StreamHdrType::HDR_TYPE_HDR10: return "HDR10";
    case StreamHdrType::HDR_TYPE_HDR10PLUS: return "HDR10+";
    case StreamHdrType::HDR_TYPE_DOLBYVISION: return "Dolby Vision";
    case StreamHdrType::HDR_TYPE_HLG: return "HLG HDR";
    case StreamHdrType::HDR_TYPE_HDR_VIVID: return "HDR Vivid";
  }
  return "";
}

std::string DoViELTypeToString(DOVIELType doviElType) {
  switch (doviElType) {
    case DOVIELType::TYPE_NONE: return "none";
    case DOVIELType::TYPE_FEL: return "full";
    case DOVIELType::TYPE_MEL: return "minimum";
  }
  return "";
}

std::string VS10ModeToString(unsigned int vs10Mode) {
  switch (vs10Mode) {
    case DOLBY_VISION_OUTPUT_MODE_IPT: return "Dolby Vision";
    case DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL: return "Dolby Vision";
    case DOLBY_VISION_OUTPUT_MODE_HDR10: return "HDR10";
    case DOLBY_VISION_OUTPUT_MODE_SDR10: return "SDR";
    case DOLBY_VISION_OUTPUT_MODE_BYPASS: return "Bypass";
  }
  return "";
}

std::string uint8_to_padded_string(uint8_t value) {
  std::stringstream ss;
  ss << std::setw(2) << std::setfill('0') << static_cast<int>(value);
  return ss.str();
}

std::string VideoDoViCodecString(CPlayerGUIInfoFrameCache& cache) {

  std::string fourCC = cache.DoViCodecFourCC();
  const DOVIStreamInfo& streamInfo = cache.DoViStreamInfo();
  uint8_t profile = streamInfo.dovi.dv_profile;
  uint8_t level = streamInfo.dovi.dv_level;

  return fmt::format("{}.{}.{}", fourCC, uint8_to_padded_string(profile), uint8_to_padded_string(level));
}

std::string FormatSampleRate(int rate) {

  // Convert to kHz
  double kHzRate = static_cast<double>(rate) / 1000.0;
  std::ostringstream oss;
  
  if (std::floor(kHzRate) == kHzRate) {
    // If it's a whole number, display without decimal places
    oss << static_cast<int>(kHzRate);
  } else if (kHzRate * 10 == std::floor(kHzRate * 10)) {
    // If it has one decimal place, display with one decimal place
    oss << std::fixed << std::setprecision(1) << kHzRate;
  } else {
    // Otherwise, display with two decimal places
    oss << std::fixed << std::setprecision(2) << kHzRate;
  }
  return oss.str();
}

std::string MakeLayoutName(uint64_t mask) {

  if (mask == 0)
    return "";

  const int total = __builtin_popcountll(mask);
  const int lfe = (mask & (1ULL << 3)) ? 1 : 0;
  const int tops = __builtin_popcountll(mask & 0x3F800ULL);
  const int bed = total - lfe - tops;

  if (tops > 0)
    return StringUtils::Format("{}.{}.{}", bed, lfe, tops);
  return StringUtils::Format("{}.{}", bed, lfe);
}

// Constants for PQ/Nits conversion
constexpr double ST2084_Y_MAX = 10000.0;
constexpr double ST2084_M1 = 2610.0 / 16384.0;
constexpr double ST2084_M2 = (2523.0 / 4096.0) * 128.0;
constexpr double ST2084_C1 = 3424.0 / 4096.0;
constexpr double ST2084_C2 = (2413.0 / 4096.0) * 32.0;
constexpr double ST2084_C3 = (2392.0 / 4096.0) * 32.0;

static double pq_to_nits(uint16_t pq) {

  // short circuit for well known PQ to nits (eliminate rounding from original 12 bit quantization)
  switch (pq) {
    case 0:    { return 0; }
    case 7:    { return 0.0001; }
    case 10:   { return 0.0002; }
    case 17:   { return 0.0005; }
    case 26:   { return 0.001; }
    case 38:   { return 0.002; }
    case 62:   { return 0.005; }
    case 3079: { return 1000.0; }
    case 3388: { return 2000.0; }
    case 3696: { return 4000.0; }
    case 4095: { return 10000.0; }
  }

  // Normalize 12-bit PQ value to 0-1 range
  double pq_normalized = pq / 4095.0;
  
  double pq_pow = std::pow(pq_normalized, 1.0 / ST2084_M2);
  double num = std::max(pq_pow - ST2084_C1, 0.0);
  double den = ST2084_C2 - ST2084_C3 * pq_pow;
  
  // Protect against division by zero
  if (std::abs(den) < std::numeric_limits<double>::epsilon()) {
    return 0.0;
  }
  
  return ST2084_Y_MAX * std::pow(num / den, 1.0 / ST2084_M1);
}

bool CPlayerGUIInfo::GetLabel(std::string& value, const CFileItem* item, int contextWindow, const CGUIInfo& info, std::string* fallback) const
{
  std::unique_lock lock(m_frameCacheSection);
  if (IsDvStreamCacheableLabel(info.m_info))
  {
    const uint32_t gen = m_frameCache->DvStreamGen();
    if (m_frameCache->TryGetLabel(info.m_info, gen, value))
      return true;
    const bool ok = GetLabelUncached(value, item, contextWindow, info, fallback);
    if (ok)
      m_frameCache->PutLabel(info.m_info, gen, value);
    return ok;
  }
  return GetLabelUncached(value, item, contextWindow, info, fallback);
}

bool CPlayerGUIInfo::GetLabelUncached(std::string& value, const CFileItem *item, int contextWindow, const CGUIInfo &info, std::string *fallback) const
{
  switch (info.m_info)
  {
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYER_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYER_SEEKOFFSET:
    {
      int lastSeekOffset = CServiceBroker::GetDataCacheCore().GetSeekOffSet();
      std::string seekOffset = StringUtils::SecondsToTimeString(
          std::abs(lastSeekOffset / 1000), static_cast<TIME_FORMAT>(info.GetData1()));
      if (lastSeekOffset < 0)
        value = "-" + seekOffset;
      else if (lastSeekOffset > 0)
        value = "+" + seekOffset;
      return true;
    }
    case PLAYER_PROGRESS:
      value = std::to_string(std::lrintf(g_application.GetPercentage()));
      return true;
    case PLAYER_PROGRESS_CACHE:
      value = std::to_string(std::lrintf(g_application.GetCachePercentage()));
      return true;
    case PLAYER_VOLUME:
      value =
          StringUtils::Format("{:2.1f} dB", CAEUtil::PercentToGain(m_appVolume->GetVolumeRatio()));
      return true;
    case PLAYER_SUBTITLE_DELAY:
      value = StringUtils::Format("{:2.3f} s", m_appPlayer->GetVideoSettings().m_SubtitleDelay);
      return true;
    case PLAYER_AUDIO_DELAY:
      value = StringUtils::Format("{:2.3f} s", m_appPlayer->GetVideoSettings().m_AudioDelay);
      return true;
    case PLAYER_CHAPTER:
      value = StringUtils::Format("{:02}", m_appPlayer->GetChapter());
      return true;
    case PLAYER_CHAPTERCOUNT:
      value = StringUtils::Format("{:02}", m_appPlayer->GetChapterCount());
      return true;
    case PLAYER_CHAPTERNAME:
      m_appPlayer->GetChapterName(value);
      return true;
    case PLAYER_PATH:
    case PLAYER_FILENAME:
    case PLAYER_FILEPATH:
      value = GUIINFO::GetFileInfoLabelValueFromPath(info.m_info, item->GetPath());
      return true;
    case PLAYER_TITLE:
    {
      // use label or drop down to title from path
      value = item->GetLabel();
      if (value.empty())
        value = CUtil::GetTitleFromPath(item->GetPath());
      return true;
    }
    case PLAYER_PLAYSPEED:
    {
      float speed = m_appPlayer->GetPlaySpeed();
      if (speed == 1.0f)
        speed = m_appPlayer->GetPlayTempo();
      value = StringUtils::Format("{:.2f}", speed);
      return true;
    }
    case PLAYER_TIME:
      value = GetCurrentPlayTime(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    case PLAYER_START_TIME:
    {
      const CDateTime time(m_appPlayer->GetStartTime());
      value = time.GetAsLocalizedTime(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    }
    case PLAYER_DURATION:
      value = GetDuration(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    case PLAYER_TIME_REMAINING:
      value = GetCurrentPlayTimeRemaining(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    case PLAYER_FINISH_TIME:
    {
      CDateTime time(CDateTime::GetCurrentDateTime());
      int playTimeRemaining = GetPlayTimeRemaining();
      float speed = m_appPlayer->GetPlaySpeed();
      float tempo = m_appPlayer->GetPlayTempo();
      if (speed == 1.0f)
        playTimeRemaining /= tempo;
      time += CDateTimeSpan(0, 0, 0, playTimeRemaining);
      value = time.GetAsLocalizedTime(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    }
    case PLAYER_TIME_SPEED:
    {
      float speed = m_appPlayer->GetPlaySpeed();
      if (speed != 1.0f)
        value = StringUtils::Format("{} ({}x)",
                                    GetCurrentPlayTime(static_cast<TIME_FORMAT>(info.GetData1())),
                                    static_cast<int>(speed));
      else
        value = GetCurrentPlayTime(TIME_FORMAT_GUESS);
      return true;
    }
    case PLAYER_SEEKTIME:
      value = GetCurrentSeekTime(static_cast<TIME_FORMAT>(info.GetData1()));
      return true;
    case PLAYER_SEEKSTEPSIZE:
    {
      int seekSize = m_appPlayer->GetSeekHandler().GetSeekSize();
      std::string strSeekSize = StringUtils::SecondsToTimeString(abs(seekSize), static_cast<TIME_FORMAT>(info.GetData1()));
      if (seekSize < 0)
        value = "-" + strSeekSize;
      if (seekSize > 0)
        value = "+" + strSeekSize;
      return true;
    }
    case PLAYER_SEEKNUMERIC:
      value = GetSeekTime(static_cast<TIME_FORMAT>(info.GetData1()));
      return !value.empty();
    case PLAYER_CACHELEVEL:
    {
      int iLevel = m_appPlayer->GetCacheLevel();
      if (iLevel >= 0)
      {
        value = std::to_string(iLevel);
        return true;
      }
      break;
    }
    case PLAYER_ITEM_ART:
      value = item->GetArt(info.GetData3());
      return true;
    case PLAYER_ICON:
      value = item->GetArt("thumb");
      if (value.empty())
        value = item->GetArt("icon");
      if (fallback)
        *fallback = item->GetArt("icon");
      return true;
    case PLAYER_EDITLIST:
    case PLAYER_CUTS:
    case PLAYER_SCENE_MARKERS:
    case PLAYER_CUTLIST:
    case PLAYER_CHAPTERS:
      value = GetContentRanges(info.m_info);
      return true;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYER_PROCESS_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYER_PROCESS_VIDEODECODER:
      value = CServiceBroker::GetDataCacheCore().GetVideoDecoderName();
      return true;
    case PLAYER_PROCESS_DEINTMETHOD:
      value = CServiceBroker::GetDataCacheCore().GetVideoDeintMethod();
      return true;
    case PLAYER_PROCESS_PIXELFORMAT:
      value = CServiceBroker::GetDataCacheCore().GetVideoPixelFormat();
      return true;
    case PLAYER_PROCESS_VIDEOFPS:
    {
      double video_fps_value = CServiceBroker::GetDataCacheCore().GetVideoFps();
      value = (std::floor(video_fps_value) == video_fps_value) ?
        StringUtils::Format("{}", video_fps_value) :
        StringUtils::Format("{:.3f}", video_fps_value);
      return true;
    }
    case PLAYER_PROCESS_VIDEODAR:
      value = StringUtils::Format("{:.2f}", CServiceBroker::GetDataCacheCore().GetVideoDAR());
      return true;
    case PLAYER_PROCESS_VIDEOWIDTH:
      value = StringUtils::FormatNumber(CServiceBroker::GetDataCacheCore().GetVideoWidth());
      return true;
    case PLAYER_PROCESS_VIDEOHEIGHT:
      value = StringUtils::FormatNumber(CServiceBroker::GetDataCacheCore().GetVideoHeight());
      return true;
    case PLAYER_PROCESS_VIDEOSCANTYPE:
      value = CServiceBroker::GetDataCacheCore().IsVideoInterlaced() ? "i" : "p";
      return true;
    case PLAYER_PROCESS_AUDIODECODER:
      value = CServiceBroker::GetDataCacheCore().GetAudioDecoderName();
      return true;
    case PLAYER_PROCESS_AUDIOCHANNELS:
      value = CServiceBroker::GetDataCacheCore().GetAudioChannels();
      return true;
    case PLAYER_PROCESS_AUDIOCHANNELS_SINK:
      value = CServiceBroker::GetDataCacheCore().GetAudioChannelsSink();
      return true;
    case PLAYER_PROCESS_AUDIO_OBJECT_COUNT:
    {
      const int objectCount = CServiceBroker::GetDataCacheCore().GetAudioObjectCount();
      if (objectCount >= 0)
      {
        value = std::to_string(objectCount);
        return true;
      }
      break;
    }
    case PLAYER_PROCESS_VIDEO_SIDEDATA:
      value = AMLGetCachedSideData();
      return !value.empty();
    case PLAYER_PROCESS_AUDIO_OBJECT_CHANNELS:
    {
      const int objectChannels = CServiceBroker::GetDataCacheCore().GetAudioObjectChannels();
      if (objectChannels > 0)
      {
        value = std::to_string(objectChannels);
        return true;
      }
      break;
    }
    case PLAYER_PROCESS_AUDIO_BED_CHANNELS:
    {
      const int bedChannels = CServiceBroker::GetDataCacheCore().GetAudioBedChannels();
      if (bedChannels > 0)
      {
        value = std::to_string(bedChannels);
        return true;
      }
      break;
    }
    case PLAYER_PROCESS_AUDIO_SPK_FL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 0)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_FR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 1)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_FC:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 2)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_LFE:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 3)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 4)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 5)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_BL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 6)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_BR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 7)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_BC:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 8)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_TFL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 11)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_TFR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 12)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_TBL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 15)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_TBR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask() & (1ULL << 16)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_FL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 0)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_FR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 1)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_FC:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 2)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_LFE:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 3)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_SL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 4)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_SR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 5)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_BL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 6)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_BR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 7)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_BC:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 8)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_TFL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 11)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_TFR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 12)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_TBL:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 15)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_SPK_SINK_TBR:
      value = (CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink() & (1ULL << 16)) ? "1" : "0";
      return true;
    case PLAYER_PROCESS_AUDIO_OBJECT_DESCRIPTION:
      value = CServiceBroker::GetDataCacheCore().GetAudioObjectDescription();
      return true;
    case PLAYER_PROCESS_AUDIO_DIALNORM:
      value = CServiceBroker::GetDataCacheCore().GetAudioDialNorm();
      return true;
    case PLAYER_PROCESS_AUDIO_LAYOUT_NAME:
      value = MakeLayoutName(CServiceBroker::GetDataCacheCore().GetAudioSpeakerMask());
      return true;
    case PLAYER_PROCESS_AUDIO_LAYOUT_NAME_SINK:
      value = MakeLayoutName(CServiceBroker::GetDataCacheCore().GetAudioSpeakerMaskSink());
      return true;
    case PLAYER_PROCESS_AUDIOSAMPLERATE:
      value = StringUtils::FormatNumber(CServiceBroker::GetDataCacheCore().GetAudioSampleRate());
      return true;
    case PLAYER_PROCESS_AUDIO_SAMPLE_RATE:
      value = FormatSampleRate(CServiceBroker::GetDataCacheCore().GetAudioSampleRate());
      return true;
    case PLAYER_PROCESS_AUDIOBITSPERSAMPLE:
      value = StringUtils::FormatNumber(CServiceBroker::GetDataCacheCore().GetAudioBitsPerSample());
      return true;

    case PLAYER_PROCESS_AUDIO_LIVE_BIT_RATE:
      value = StringUtils::Format(
          "{:.1f}", CServiceBroker::GetDataCacheCore().GetAudioLiveBitRate() / 1000.0);
      value += " " + g_localizeStrings.Get(25019);
      return true;
    case PLAYER_PROCESS_AUDIO_LIVE_KIBIT_RATE:
      value = StringUtils::FormatNumber((CServiceBroker::GetDataCacheCore().GetAudioLiveBitRate() / 1024), 0);
      return true;
    case PLAYER_PROCESS_AUDIO_LIVE_MIBIT_RATE:
      value = StringUtils::FormatNumber((CServiceBroker::GetDataCacheCore().GetAudioLiveBitRate() / 1048576), 2);
      return true;
    case PLAYER_PROCESS_AUDIO_QUEUE_LEVEL:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetAudioQueueLevel());
      return true;
    case PLAYER_PROCESS_AUDIO_QUEUE_DATA_LEVEL:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetAudioQueueDataLevel());
      return true;
    case PLAYER_PROCESS_VIDEO_LIVE_BIT_RATE:
      value = StringUtils::Format(
          "{:.1f}", CServiceBroker::GetDataCacheCore().GetVideoLiveBitRate() / 1000000.0);
      value += " " + g_localizeStrings.Get(25020);
      return true;
    case PLAYER_PROCESS_VIDEO_LIVE_KIBIT_RATE:
      value = StringUtils::FormatNumber((CServiceBroker::GetDataCacheCore().GetVideoLiveBitRate() / 1024), 0);
      return true;
    case PLAYER_PROCESS_VIDEO_LIVE_MIBIT_RATE:
      value = StringUtils::FormatNumber((CServiceBroker::GetDataCacheCore().GetVideoLiveBitRate() / 1048576), 2);
      return true;
    case PLAYER_PROCESS_VIDEO_QUEUE_LEVEL:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoQueueLevel());
      return true;
    case PLAYER_PROCESS_VIDEO_QUEUE_DATA_LEVEL:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoQueueDataLevel());
      return true;
    case PLAYER_PROCESS_VIDEO_BIT_DEPTH:
      value = StringUtils::FormatNumber(CServiceBroker::GetDataCacheCore().GetVideoBitDepth());
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_TYPE:
      value = HdrTypeToString(CServiceBroker::GetDataCacheCore().GetVideoHdrType());
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_TYPE_RAW:
      value = std::to_string(static_cast<int>(CServiceBroker::GetDataCacheCore().GetVideoHdrType()));
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_HDR_TYPE:
      value = HdrTypeToString(CServiceBroker::GetDataCacheCore().GetVideoSourceHdrType());
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_HDR_TYPE_RAW:
      value = std::to_string(static_cast<int>(CServiceBroker::GetDataCacheCore().GetVideoSourceHdrType()));
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_ADDITIONAL_HDR_TYPE:
      value = HdrTypeToString(CServiceBroker::GetDataCacheCore().GetVideoSourceAdditionalHdrType());
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_ADDITIONAL_HDR_TYPE_RAW:
      value = std::to_string(static_cast<int>(CServiceBroker::GetDataCacheCore().GetVideoSourceAdditionalHdrType()));
      return true;
    case PLAYER_PROCESS_VIDEO_WIDTH_RAW:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoWidth());
      return true;
    case PLAYER_PROCESS_VIDEO_HEIGHT_RAW:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoHeight());
      return true;
    case PLAYER_PROCESS_VIDEO_COLOR_SPACE:
      value = av_color_space_name(CServiceBroker::GetDataCacheCore().GetVideoColorSpace());
      return true;
    case PLAYER_PROCESS_VIDEO_COLOR_RANGE:
      value = av_color_range_name(CServiceBroker::GetDataCacheCore().GetVideoColorRange());
      return true;
    case PLAYER_PROCESS_VIDEO_COLOR_PRIMARIES:
      value = av_color_primaries_name(CServiceBroker::GetDataCacheCore().GetVideoColorPrimaries());
      return true;
    case PLAYER_PROCESS_VIDEO_COLOR_TRANSFER_CHARACTERISTIC:
      value = av_color_transfer_name(CServiceBroker::GetDataCacheCore().GetVideoColorTransferCharacteristic());
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_HAS_CONFIG:
      value = std::to_string(m_frameCache->DoViStreamInfo().has_config);
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_VERSION_MAJOR:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.dv_version_major);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_VERSION_MINOR:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.dv_version_minor);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_PROFILE:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.dv_profile);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_LEVEL:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.dv_level);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_RPU_PRESENT:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.rpu_present_flag);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_EL_PRESENT:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.el_present_flag);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_BL_PRESENT:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.bl_present_flag);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_BL_SIGNAL_COMPATIBILITY:
      value = std::to_string(m_frameCache->DoViStreamInfo().dovi.dv_bl_signal_compatibility_id);
      return true;

    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_PROFILE:
      value = std::to_string(m_frameCache->SourceDoViStreamInfo().dovi.dv_profile);
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_BL_SIGNAL_COMPATIBILITY:
      value = std::to_string(m_frameCache->SourceDoViStreamInfo().dovi.dv_bl_signal_compatibility_id);
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_EL_PRESENT:
      value = std::to_string(m_frameCache->SourceDoViStreamInfo().dovi.el_present_flag);
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_EL_TYPE:
      value = DoViELTypeToString(m_frameCache->SourceDoViStreamInfo().dovi_el_type);
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_DOVI_META_VERSION:
      value = m_frameCache->DoViFrame().source_meta_version;
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_CODEC_FOURCC:
      value = m_frameCache->DoViCodecFourCC();
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_CODEC_STRING:
      value = VideoDoViCodecString(*m_frameCache);
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_EL_TYPE:
      value = DoViELTypeToString(m_frameCache->DoViStreamInfo().dovi_el_type);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_META_VERSION:
      value = m_frameCache->DoViStreamMeta().meta_version;
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_HAS_HEADER:
      value = std::to_string(m_frameCache->DoViStreamInfo().has_header);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_DUAL_TRACK:
    {
      const auto& info = m_frameCache->DoViStreamInfo();
      if (info.dovi_el_type == DOVIELType::TYPE_NONE)
        value = "";
      else
        value = info.is_dual_track ? "DT-DL" : "ST-DL";
      return true;
    }
    case PLAYER_PROCESS_VIDEO_HDMI_OUTPUT:
    {
      std::string cs = GetAMLConfigInfo("Colourspace");
      std::string cd = GetAMLConfigInfo("Colour depth");
      if (cs == "unknown" || cd == "unknown")
      {
        value = "";
      }
      else
      {
        StringUtils::Replace(cs, "YUV444", "4:4:4");
        StringUtils::Replace(cs, "YUV422", "4:2:2");
        StringUtils::Replace(cs, "YUV420", "4:2:0");
        value = cs + ", " + cd;
      }
      return true;
    }

    case PLAYER_PROCESS_VIDEO_DOVI_L1_MIN_PQ:
      value = std::to_string(m_frameCache->DoViFrame().level1_min_pq);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L1_MAX_PQ:
      value = std::to_string(m_frameCache->DoViFrame().level1_max_pq);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L1_AVG_PQ:
      value = std::to_string(m_frameCache->DoViFrame().level1_avg_pq);
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_L1_MIN_NITS:
      value = StringUtils::FormatNumber(pq_to_nits(m_frameCache->DoViFrame().level1_min_pq), 4);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L1_MAX_NITS:
      value = std::to_string(static_cast<int>(pq_to_nits(m_frameCache->DoViFrame().level1_max_pq)));
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L1_AVG_NITS:
      value = std::to_string(static_cast<int>(pq_to_nits(m_frameCache->DoViFrame().level1_avg_pq)));
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_HAS_L5:
      value =std::to_string(m_frameCache->DoViFrame().has_level5_metadata);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L5_LEFT_OFFSET:
      value = std::to_string(m_frameCache->DoViFrame().level5_active_area_left_offset);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L5_RIGHT_OFFSET:
      value = std::to_string(m_frameCache->DoViFrame().level5_active_area_right_offset);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L5_TOP_OFFSET:
      value = std::to_string(m_frameCache->DoViFrame().level5_active_area_top_offset);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L5_BOTTOM_OFFSET:
      value = std::to_string(m_frameCache->DoViFrame().level5_active_area_bottom_offset);
      return true;
    case PLAYER_PROCESS_VIDEO_ACTIVE_AREA_TOP:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoActiveAreaTop());
      return true;
    case PLAYER_PROCESS_VIDEO_ACTIVE_AREA_BOTTOM:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoActiveAreaBottom());
      return true;
    case PLAYER_PROCESS_VIDEO_ACTIVE_AREA_TOP_LINES:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoActiveAreaTopLines());
      return true;
    case PLAYER_PROCESS_VIDEO_ACTIVE_AREA_BOTTOM_LINES:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetVideoActiveAreaBottomLines());
      return true;

    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MIN_PQ:
      value = std::to_string(m_frameCache->DoViStreamMeta().source_min_pq);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MAX_PQ:
      value = std::to_string(m_frameCache->DoViStreamMeta().source_max_pq);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MIN_NITS:
      value = StringUtils::FormatNumber(pq_to_nits(m_frameCache->DoViStreamMeta().source_min_pq), 4);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_SOURCE_MAX_NITS:
      value = std::to_string(static_cast<int>(pq_to_nits(m_frameCache->DoViStreamMeta().source_max_pq)));
      return true;
  
    case PLAYER_PROCESS_VIDEO_DOVI_HAS_L6:
      value = std::to_string(m_frameCache->DoViStreamMeta().has_level6_metadata);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_CLL:
      value = std::to_string(m_frameCache->DoViStreamMeta().level6_max_cll);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_FALL:
      value = std::to_string(m_frameCache->DoViStreamMeta().level6_max_fall);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MIN_LUM:
      value = StringUtils::FormatNumber((m_frameCache->DoViStreamMeta().level6_min_lum * 0.0001), 4);
      return true;
    case PLAYER_PROCESS_VIDEO_DOVI_L6_MAX_LUM:
      value = std::to_string(m_frameCache->DoViStreamMeta().level6_max_lum);
      return true;    

    case PLAYER_PROCESS_VIDEO_HDR_HAS_CLL:
      value = std::to_string(m_frameCache->HdrStatic().has_cll_metadata);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MAX_CLL:
      value = std::to_string(m_frameCache->HdrStatic().max_cll);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MAX_FALL:
      value = std::to_string(m_frameCache->HdrStatic().max_fall);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_HAS_MDCV:
      value = std::to_string(m_frameCache->HdrStatic().has_mdcv_metadata);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MIN_LUM:
      value = StringUtils::FormatNumber((m_frameCache->HdrStatic().min_lum * 0.0001), 4);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MAX_LUM:
      value = std::to_string(m_frameCache->HdrStatic().max_lum);
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MAX_CLL_LIMITED:
      value = GetHdr10LimitedValue(
          m_frameCache->HdrStatic().max_cll,
          static_cast<int>(CServiceBroker::GetDataCacheCore().GetHdr10OverrideMaxCll()),
          static_cast<int>(CServiceBroker::GetDataCacheCore().GetDvLevel6MaxCll()));
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_MAX_LUM_LIMITED:
      value = GetHdr10LimitedValue(
          m_frameCache->HdrStatic().max_lum,
          static_cast<int>(CServiceBroker::GetDataCacheCore().GetHdr10OverrideMaxLum()),
          static_cast<int>(CServiceBroker::GetDataCacheCore().GetDvLevel6MaxLum()));
      return true;
    case PLAYER_PROCESS_VIDEO_HDR_COLOUR_PRIMARIES:
      value = m_frameCache->HdrStatic().colour_primaries;
      return true;

    case PLAYER_PROCESS_AML_PIXELFORMAT:
      value = GetAMLConfigInfo("Colour depth") + ", " + GetAMLConfigInfo("Colourspace");
      return true;
    case PLAYER_PROCESS_AML_DISPLAYMODE:
      value =  GetAMLConfigInfo("VIC");
      return true;
    case PLAYER_PROCESS_AML_EOFT_GAMUT:
      value = GetAMLConfigInfo("EOTF") + " " + GetAMLConfigInfo("Colourimetry");
      return true;
    case PLAYER_PROCESS_AML_VS10_MODE:
      value = VS10ModeToString(m_frameCache->DvMode());
      return true;
    case PLAYER_PROCESS_AML_VS10_MODE_RAW:
      value = std::to_string(m_frameCache->DvMode());
      return true;
    case PLAYER_PROCESS_AML_DV_TYPE_RAW:
      value = std::to_string(static_cast<int>(aml_dv_type()));
      return true;
    case PLAYER_PROCESS_AML_VIDEO_FPS_INFO:
      value = aml_video_fps_info();
      return true;
    case PLAYER_PROCESS_AML_VIDEO_FPS_DROP:
      value = aml_video_fps_drop();
      return true;

    case PLAYER_PROCESS_AV_CHANGE:
      value = std::to_string(CServiceBroker::GetDataCacheCore().GetAVChange());
      return true;
    case PLAYER_PROCESS_RENDER_PTS:
      value = std::to_string(static_cast<int64_t>(CServiceBroker::GetDataCacheCore().GetRenderPts()));
      return true;

    case PLAYER_PROCESS_AML_VP_MODE:
      value = std::to_string(m_frameCache->VpMode() != 0 ? 1 : 0);
      return true;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYLIST_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYLIST_LENGTH:
    case PLAYLIST_POSITION:
    case PLAYLIST_RANDOM:
    case PLAYLIST_REPEAT:
      value = GUIINFO::GetPlaylistLabel(info.m_info, info.GetData1());
      return true;
  }

  return false;
}

bool CPlayerGUIInfo::GetInt(int& value, const CGUIListItem *gitem, int contextWindow, const CGUIInfo &info) const
{
  switch (info.m_info)
  {
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYER_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYER_VOLUME:
      value = static_cast<int>(m_appVolume->GetVolumePercent());
      return true;
    case PLAYER_PROGRESS:
      value = std::lrintf(g_application.GetPercentage());
      return true;
    case PLAYER_PROGRESS_CACHE:
      value = std::lrintf(g_application.GetCachePercentage());
      return true;
    case PLAYER_SEEKBAR:
      value = std::lrintf(GetSeekPercent());
      return true;
    case PLAYER_CACHELEVEL:
      value = m_appPlayer->GetCacheLevel();
      return true;
    case PLAYER_CHAPTER:
      value = m_appPlayer->GetChapter();
      return true;
    case PLAYER_CHAPTERCOUNT:
      value = m_appPlayer->GetChapterCount();
      return true;
    case PLAYER_SUBTITLE_DELAY:
      value = m_appPlayer->GetSubtitleDelay();
      return true;
    case PLAYER_AUDIO_DELAY:
      value = m_appPlayer->GetAudioDelay();
      return true;
    case PLAYER_PROCESS_AUDIO_QUEUE_LEVEL:
      value = CServiceBroker::GetDataCacheCore().GetAudioQueueLevel();
      return true;
    case PLAYER_PROCESS_AUDIO_QUEUE_DATA_LEVEL:
      value = CServiceBroker::GetDataCacheCore().GetAudioQueueDataLevel();
      return true;
    case PLAYER_PROCESS_VIDEO_QUEUE_LEVEL:
      value = CServiceBroker::GetDataCacheCore().GetVideoQueueLevel();
      return true;
    case PLAYER_PROCESS_VIDEO_QUEUE_DATA_LEVEL:
      value = CServiceBroker::GetDataCacheCore().GetVideoQueueDataLevel();
      return true;
    case PLAYER_PROCESS_VIDEO_SOURCE_HDR_TYPE_RAW:
      value = static_cast<int>(CServiceBroker::GetDataCacheCore().GetVideoSourceHdrType());
      return true;
  }

  return false;
}

bool CPlayerGUIInfo::GetBool(bool& value, const CGUIListItem *gitem, int contextWindow, const CGUIInfo &info) const
{
  const CFileItem *item = nullptr;
  if (gitem->IsFileItem())
    item = static_cast<const CFileItem*>(gitem);

  switch (info.m_info)
  {
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYER_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYER_SHOWINFO:
      value = m_playerShowInfo;
      return true;
    case PLAYER_SHOWTIME:
      value = m_playerShowTime;
      return true;
    case PLAYER_MUTED:
      value = (m_appVolume->IsMuted() ||
               m_appVolume->GetVolumeRatio() <= CApplicationVolumeHandling::VOLUME_MINIMUM);
      return true;
    case PLAYER_HAS_MEDIA:
      value = m_appPlayer->IsPlaying();
      return true;
    case PLAYER_HAS_AUDIO:
      value = m_appPlayer->IsPlayingAudio();
      return true;
    case PLAYER_HAS_VIDEO:
      value = m_appPlayer->IsPlayingVideo();
      return true;
    case PLAYER_HAS_GAME:
      value = m_appPlayer->IsPlayingGame();
      return true;
    case PLAYER_IS_REMOTE:
      value = m_appPlayer->IsRemotePlaying();
      return true;
    case PLAYER_IS_EXTERNAL:
      value = m_appPlayer->IsExternalPlaying();
      return true;
    case PLAYER_PLAYING:
      value = m_appPlayer->GetPlaySpeed() == 1.0f;
      return true;
    case PLAYER_PAUSED:
      value = m_appPlayer->IsPausedPlayback();
      return true;
    case PLAYER_REWINDING:
      value = m_appPlayer->GetPlaySpeed() < 0.0f;
      return true;
    case PLAYER_FORWARDING:
      value = m_appPlayer->GetPlaySpeed() > 1.5f;
      return true;
    case PLAYER_REWINDING_2x:
      value = m_appPlayer->GetPlaySpeed() == -2;
      return true;
    case PLAYER_REWINDING_4x:
      value = m_appPlayer->GetPlaySpeed() == -4;
      return true;
    case PLAYER_REWINDING_8x:
      value = m_appPlayer->GetPlaySpeed() == -8;
      return true;
    case PLAYER_REWINDING_16x:
      value = m_appPlayer->GetPlaySpeed() == -16;
      return true;
    case PLAYER_REWINDING_32x:
      value = m_appPlayer->GetPlaySpeed() == -32;
      return true;
    case PLAYER_FORWARDING_2x:
      value = m_appPlayer->GetPlaySpeed() == 2;
      return true;
    case PLAYER_FORWARDING_4x:
      value = m_appPlayer->GetPlaySpeed() == 4;
      return true;
    case PLAYER_FORWARDING_8x:
      value = m_appPlayer->GetPlaySpeed() == 8;
      return true;
    case PLAYER_FORWARDING_16x:
      value = m_appPlayer->GetPlaySpeed() == 16;
      return true;
    case PLAYER_FORWARDING_32x:
      value = m_appPlayer->GetPlaySpeed() == 32;
      return true;
    case PLAYER_CAN_PAUSE:
      value = m_appPlayer->CanPause();
      return true;
    case PLAYER_CAN_SEEK:
      value = m_appPlayer->CanSeek();
      return true;
    case PLAYER_SUPPORTS_TEMPO:
      value = m_appPlayer->SupportsTempo();
      return true;
    case PLAYER_IS_TEMPO:
      value = (m_appPlayer->GetPlayTempo() != 1.0f && m_appPlayer->GetPlaySpeed() == 1.0f);
      return true;
    case PLAYER_CACHING:
      value = m_appPlayer->IsCaching();
      return true;
    case PLAYER_SEEKBAR:
    {
      CGUIDialog *seekBar = CServiceBroker::GetGUI()->GetWindowManager().GetDialog(WINDOW_DIALOG_SEEK_BAR);
      value = seekBar ? seekBar->IsDialogRunning() : false;
      return true;
    }
    case PLAYER_SEEKING:
      value = m_appPlayer->GetSeekHandler().InProgress();
      return true;
    case PLAYER_HASPERFORMEDSEEK:
    {
      int requestedLastSecondInterval{0};
      std::from_chars_result result =
          std::from_chars(info.GetData3().data(), info.GetData3().data() + info.GetData3().size(),
                          requestedLastSecondInterval);
      if (result.ec == std::errc::invalid_argument)
      {
        value = false;
        return false;
      }

      value = CServiceBroker::GetDataCacheCore().HasPerformedSeek(requestedLastSecondInterval);
      return true;
    }
    case PLAYER_PASSTHROUGH:
      value = m_appPlayer->IsPassthrough();
      return true;
    case PLAYER_ISINTERNETSTREAM:
      if (item)
      {
        value = URIUtils::IsInternetStream(item->GetDynPath());
        return true;
      }
      break;
    case PLAYER_HAS_PROGRAMS:
      value = (m_appPlayer->GetProgramsCount() > 1) ? true : false;
      return true;
    case PLAYER_HAS_RESOLUTIONS:
      value = CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot() &&
              CResolutionUtils::HasWhitelist();
      return true;
    case PLAYER_HASDURATION:
      value = g_application.GetTotalTime() > 0;
      return true;
    case PLAYER_FRAMEADVANCE:
      value = CServiceBroker::GetDataCacheCore().IsFrameAdvance();
      return true;
    case PLAYER_HAS_SCENE_MARKERS:
      value = !CServiceBroker::GetDataCacheCore().GetSceneMarkers().empty();
      return true;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYLIST_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYLIST_ISRANDOM:
    {
      PLAYLIST::CPlayListPlayer& player = CServiceBroker::GetPlaylistPlayer();
      PLAYLIST::Id playlistid = info.GetData1();
      if (info.GetData2() > 0 && playlistid != PLAYLIST::TYPE_NONE)
        value = player.IsShuffled(playlistid);
      else
        value = player.IsShuffled(player.GetCurrentPlaylist());
      return true;
    }
    case PLAYLIST_ISREPEAT:
    {
      PLAYLIST::CPlayListPlayer& player = CServiceBroker::GetPlaylistPlayer();
      PLAYLIST::Id playlistid = info.GetData1();
      if (info.GetData2() > 0 && playlistid != PLAYLIST::TYPE_NONE)
        value = (player.GetRepeat(playlistid) == PLAYLIST::RepeatState::ALL);
      else
        value = player.GetRepeat(player.GetCurrentPlaylist()) == PLAYLIST::RepeatState::ALL;
      return true;
    }
    case PLAYLIST_ISREPEATONE:
    {
      PLAYLIST::CPlayListPlayer& player = CServiceBroker::GetPlaylistPlayer();
      PLAYLIST::Id playlistid = info.GetData1();
      if (info.GetData2() > 0 && playlistid != PLAYLIST::TYPE_NONE)
        value = (player.GetRepeat(playlistid) == PLAYLIST::RepeatState::ONE);
      else
        value = player.GetRepeat(player.GetCurrentPlaylist()) == PLAYLIST::RepeatState::ONE;
      return true;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // PLAYER_PROCESS_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case PLAYER_PROCESS_VIDEOHWDECODER:
      value = CServiceBroker::GetDataCacheCore().IsVideoHwDecoder();
      return true;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // LISTITEM_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case LISTITEM_ISPLAYING:
    {
      if (item)
      {
        if (item->HasProperty("playlistposition"))
        {
          value = static_cast<int>(item->GetProperty("playlisttype").asInteger()) ==
                      CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() &&
                  static_cast<int>(item->GetProperty("playlistposition").asInteger()) ==
                      CServiceBroker::GetPlaylistPlayer().GetCurrentItemIdx();
          return true;
        }
        else if (m_currentItem && !m_currentItem->GetPath().empty())
        {
          if (!g_application.m_strPlayListFile.empty())
          {
            //playlist file that is currently playing or the playlistitem that is currently playing.
            value = item->IsPath(g_application.m_strPlayListFile) || m_currentItem->IsSamePath(item);
          }
          else
          {
            value = m_currentItem->IsSamePath(item);
          }
          return true;
        }
      }
      break;
    }
  }

  return false;
}

std::string CPlayerGUIInfo::GetContentRanges(int iInfo) const
{
  std::string values;

  CDataCacheCore& data = CServiceBroker::GetDataCacheCore();
  std::vector<std::pair<float, float>> ranges;

  std::time_t start;
  int64_t current;
  int64_t min;
  int64_t max;
  data.GetPlayTimes(start, current, min, max);

  std::time_t duration = max - start * 1000;
  if (duration > 0)
  {
    switch (iInfo)
    {
      case PLAYER_EDITLIST:
      case PLAYER_CUTLIST:
        ranges = GetEditList(data, duration);
        break;
      case PLAYER_CUTS:
        ranges = GetCuts(data, duration);
        break;
      case PLAYER_SCENE_MARKERS:
        ranges = GetSceneMarkers(data, duration);
        break;
      case PLAYER_CHAPTERS:
        ranges = GetChapters(data, duration);
        break;
      default:
        CLog::Log(LOGERROR, "CPlayerGUIInfo::GetContentRanges({}) - unhandled guiinfo", iInfo);
        break;
    }

    // create csv string from ranges
    for (const auto& range : ranges)
      values += StringUtils::Format("{:.5f},{:.5f},", range.first, range.second);

    if (!values.empty())
      values.pop_back(); // remove trailing comma
  }

  return values;
}

std::vector<std::pair<float, float>> CPlayerGUIInfo::GetEditList(const CDataCacheCore& data,
                                                                 std::time_t duration) const
{
  std::vector<std::pair<float, float>> ranges;

  const std::vector<EDL::Edit>& edits = data.GetEditList();
  for (const auto& edit : edits)
  {
    float editStart = edit.start * 100.0f / duration;
    float editEnd = edit.end * 100.0f / duration;
    ranges.emplace_back(editStart, editEnd);
  }
  return ranges;
}

std::vector<std::pair<float, float>> CPlayerGUIInfo::GetCuts(const CDataCacheCore& data,
                                                             std::time_t duration) const
{
  std::vector<std::pair<float, float>> ranges;

  const std::vector<int64_t>& cuts = data.GetCuts();
  float lastMarker = 0.0f;
  for (const auto& cut : cuts)
  {
    float marker = static_cast<float>(cut) * 100.0f / static_cast<float>(duration);

    if (marker >= 100.0f)
      break;

    if (marker != 0.0f)
      ranges.emplace_back(lastMarker, marker);

    lastMarker = marker;
  }
  return ranges;
}

std::vector<std::pair<float, float>> CPlayerGUIInfo::GetSceneMarkers(const CDataCacheCore& data,
                                                                     std::time_t duration) const
{
  std::vector<std::pair<float, float>> ranges;

  const std::vector<int64_t>& scenes = data.GetSceneMarkers();
  float lastMarker = 0.0f;
  for (const auto& scene : scenes)
  {
    float marker = scene * 100.0f / duration;
    if (marker != 0)
      ranges.emplace_back(lastMarker, marker);

    lastMarker = marker;
  }
  return ranges;
}

std::vector<std::pair<float, float>> CPlayerGUIInfo::GetChapters(const CDataCacheCore& data,
                                                                 std::time_t duration) const
{
  std::vector<std::pair<float, float>> ranges;

  const std::vector<std::pair<std::string, int64_t>>& chapters = data.GetChapters();
  float lastMarker = 0.0f;
  for (const auto& chapter : chapters)
  {
    float marker = chapter.second * 1000 * 100.0f / duration;
    if (marker != 0)
      ranges.emplace_back(lastMarker, marker);

    lastMarker = marker;
  }
  return ranges;
}
