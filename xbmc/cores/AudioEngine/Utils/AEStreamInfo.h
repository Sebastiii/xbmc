/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AEPackIEC61937.h"
#include "AEChannelInfo.h"
#include <stdint.h>
#include <array>
#include <string>

/* ffmpeg re-defines this, so undef it to squash the warning */
#undef restrict

extern "C" {
#include <libavutil/crc.h>
}

class CAEStreamInfo
{
public:
  double GetDuration() const;
  bool operator==(const CAEStreamInfo& info) const;

  enum DataType
  {
    STREAM_TYPE_NULL,
    STREAM_TYPE_AC3,
    STREAM_TYPE_DTS_512,
    STREAM_TYPE_DTS_1024,
    STREAM_TYPE_DTS_2048,
    STREAM_TYPE_DTSHD,
    STREAM_TYPE_DTSHD_CORE,
    STREAM_TYPE_EAC3,
    STREAM_TYPE_MLP,
    STREAM_TYPE_TRUEHD,
    STREAM_TYPE_DTSHD_MA
  };
  DataType m_type = STREAM_TYPE_NULL;
  unsigned int m_sampleRate = 0;
  unsigned int m_bitDepth = 0;
  unsigned int m_channels = 0;
  bool m_dataIsLE = true;
  unsigned int m_dtsPeriod = 0;
  unsigned int m_repeat = 0;
  unsigned int m_ac3FrameSize = 0;
  unsigned int m_dtsSamplesPerFrame = 0;
  int m_dialNorm = 0; // Dialog Normalization in dB (TrueHD Atmos 16ch)
  int m_dialNormApplied = 0;
  bool m_hasDialNorm = false;
  bool m_hasAtmos = false; // TrueHD Atmos (16-channel presentation)
  bool m_hasDtsX = false;
  unsigned int m_atmosChannels = 0; // Atmos 16ch channel/object count
  int m_atmosObjects = -1;
  int m_bedChannels = -1;
  bool m_bedIsLfeOnly = false;
};

class CAEStreamParser
{
public:

  CAEStreamParser();
  ~CAEStreamParser() = default;

  int AddData(uint8_t *data, unsigned int size, uint8_t **buffer = nullptr, unsigned int *bufferSize = nullptr);

  enum class SyncFamily
  {
    Any,
    AC3,
    DTS,
    TrueHD
  };
  void SetSyncFamily(SyncFamily family) { m_syncFamily = family; }

  void SetCoreOnly(bool value) { m_coreOnly = value; }
  void SetDefeatTrueHDDialNorm(bool value) { m_defeatTrueHDDialNorm = value; }
  void SetDefeatAC3DialNorm(bool value) { m_defeatAC3DialNorm = value; }
  void SetDefeatDTSDialNorm(bool value) { m_defeatDTSDialNorm = value; }
  void SetEAC3JOC(bool value) { m_eac3IsJOC = value; }
  unsigned int IsValid() const { return m_hasSync; }
  unsigned int GetSampleRate() const { return m_info.m_sampleRate; }
  unsigned int GetChannels() const { return m_info.m_channels; }
  unsigned int GetFrameSize() const { return m_fsize; }
  // unsigned int GetDTSBlocks() const { return m_dtsBlocks; }
  unsigned int GetDTSPeriod() const { return m_info.m_dtsPeriod; }
  unsigned int GetEAC3BlocksDiv() const { return m_info.m_repeat; }
  enum CAEStreamInfo::DataType GetDataType() const { return m_info.m_type; }
  int GetDialNorm() const { return m_info.m_dialNorm; }
  int GetDialNormApplied() const { return m_info.m_dialNormApplied; }
  bool HasDialNorm() const { return m_info.m_hasDialNorm; }
  bool HasAtmos() const { return m_info.m_hasAtmos; }
  bool HasDtsX() const { return m_info.m_hasDtsX; }
  void SetDtsX(bool value) { m_dtsX = value; }
  unsigned int GetAtmosChannels() const { return m_info.m_atmosChannels; }
  int GetAtmosObjects() const { return m_info.m_atmosObjects; }
  bool IsLittleEndian() const { return m_info.m_dataIsLE; }
  unsigned int GetBufferSize() const { return m_bufferSize; }
  CAEStreamInfo& GetStreamInfo() { return m_info; }
  void Reset();

private:
  uint8_t m_buffer[MAX_IEC61937_PACKET];
  unsigned int m_bufferSize = 0;
  unsigned int m_skipBytes = 0;

  typedef unsigned int (CAEStreamParser::*ParseFunc)(uint8_t *data, unsigned int size);

  CAEStreamInfo m_info;
  SyncFamily m_syncFamily = SyncFamily::Any;
  bool m_coreOnly = false;
  bool m_defeatTrueHDDialNorm = false;
  bool m_defeatAC3DialNorm = false;
  bool m_defeatDTSDialNorm = false;
  bool m_eac3IsJOC = false;
  bool m_dtsX = false;
  bool m_eac3ObjectsLatched = false;
  unsigned int m_eac3ScanAttempts = 0;
  static constexpr unsigned int EAC3_OBJECT_SCANS_UNTYPED = 64;
  static constexpr unsigned int EAC3_OBJECT_SCANS_TYPED_ATMOS = 512;
  unsigned int EAC3ObjectScanBudget() const
  {
    return m_eac3IsJOC ? EAC3_OBJECT_SCANS_TYPED_ATMOS : EAC3_OBJECT_SCANS_UNTYPED;
  }
  unsigned int m_needBytes = 0;
  ParseFunc m_syncFunc;
  bool m_hasSync = false;

  std::string m_lastLoggedStreamDetected;
  std::array<int, 10> m_lastTrueHDLogKey{};
  bool m_hasTrueHDLogKey = false;

  unsigned int m_coreSize = 0;         /* core size for dtsHD */
  unsigned int m_dtsBlocks = 0;
  unsigned int m_dtsChangeStreak = 0;
  CAEStreamInfo::DataType m_dtsCandidateType = CAEStreamInfo::STREAM_TYPE_NULL;
  unsigned int m_dtsCandidateRate = 0;
  unsigned int m_dtsCandidateBlocks = 0;
  unsigned int m_fsize = 0;
  int m_substreams = 0;       /* used for TrueHD  */
  AVCRC m_crcTrueHD[1024];  /* TrueHD crc table */

  void GetPacket(uint8_t **buffer, unsigned int *bufferSize);
  void DefeatAC3DialNorm(uint8_t* data, unsigned int size);
  void DefeatDTSDialNorm(uint8_t* data, unsigned int size);
  unsigned int DetectType(uint8_t *data, unsigned int size);
  bool TrySyncAC3(uint8_t *data, unsigned int size, bool resyncing, bool wantEAC3dependent);
  unsigned int SyncAC3(uint8_t *data, unsigned int size);
  unsigned int SyncDTS(uint8_t *data, unsigned int size);
  unsigned int SyncTrueHD(uint8_t *data, unsigned int size);

  static unsigned int GetTrueHDChannels(const uint16_t chanmap);
};