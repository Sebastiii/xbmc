/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  Object audio and DTS extension substream parsing is implemented from the
 *  published specifications:
 *
 *    ETSI TS 103 420 V1.2.1 (object audio carriage using Enhanced AC-3)
 *      program_assignment syntax      clause 5.5.3
 *      content_description elements   table 11a
 *      intermediate spatial format    table 11b
 *      bed channel assignment widths  table 12
 *      non standard bed assignment    table 13
 *      num_bed_instances              clauses 5.6.0.9 and 5.6.0.10
 *      num_dynamic_objects            clause 5.6.0.12
 *      reserved_data_size             clause 5.6.4.1
 *
 *    ETSI TS 102 114 V1.6.1 (DTS coherent acoustics)
 *      extension substream header     table 7-2
 *      audio asset descriptor         table 7-5
 *      loudspeaker activity mask      table 7-10
 *
 *  TS 103 420 specifies an extension to E-AC-3 only. The TrueHD 16 channel
 *  presentation carries the same program assignment fields but is not covered
 *  by a published specification, so its intermediate spatial format indices are
 *  restricted to those the format is known to use.
 *
 *  No published specification documents the DTS:X height layer. A stream that
 *  ffmpeg reports as DTS:X, and whose loudspeaker activity mask declares no
 *  height channels, is modelled as carrying four.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEStreamInfo.h"

#include "utils/LogThrottle.h"
#include "utils/log.h"

#include <algorithm>
#include <climits>
#include <fmt/format.h>
#include <string.h>
#include <iomanip>
#include <sstream>

// Reference for DTS and DTS-UHD (aka DTS:X)
// https://www.etsi.org/deliver/etsi_ts/102100_102199/102114/01.06.01_60/ts_102114v010601p.pdf
// https://www.etsi.org/deliver/etsi_ts/103400_103499/103491/01.02.01_60/ts_103491v010201p.pdf

#define DTS_SYNC_CORE_14BE  0x1FFFE800
#define DTS_SYNC_CORE_14LE  0xFF1F00E8  // DTS CD - upto 5.1 on CD!
#define DTS_SYNC_CORE_16BE  0x7FFE8001  // DTS on DVD / BluRay
#define DTS_SYNC_CORE_16LE  0xFE7F0180

#define DTS_SYNC_EXTENTION  0x64582025  // DTS Extention Subsystem for below extensions.

static constexpr unsigned int DTS_CHANGE_CONFIRM_FRAMES = 3;
static constexpr unsigned int DTSHD_PACKER_OVERHEAD = 12;

static bool IsPackableDtsBurst(unsigned int period)
{
  return period == 512 || period == 1024 || period == 2048 || period == 4096 ||
         period == 8192 || period == 16384;
}

#define DTS_SYNC_EXT_XCH    0x5a5a5a5a  // DTS Extension to 6.1 Channels (XCh) - in case of multiple extension streams the XCh stream is always the last.
#define DTS_SYNC_EXT_XXCH   0x47004a03  // DTS Extension to More Than 5.1 Channels (XXCh)
#define DTS_SYNC_EXT_X96K   0x1d95f262  // DTS Extension to 96 kHz Frequency (X96k) - if a channel extension is present the X96k extension data is placed before the XCh extension data in the encoded bit stream.
#define DTS_SYNC_EXT_XBR    0x655e315e  // DTS Extension Extended Bit Rate, allow greater than 1.5 Mbps
#define DTS_SYNC_EXT_LBR    0x0a801921  // DTS Extention Low Bit Rate
#define DTS_SYNC_EXT_XLL    0x41a29547  // DTS Extention Lossless conding extension as used for DTS-HD Master Audio

#define DTS_SFREQ_COUNT 16
#define MAX_EAC3_BLOCKS 6
#define UNKNOWN_DTS_EXTENSION 255

static const uint16_t AC3Bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                       192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod[] = {48000, 44100, 32000, 0};
static const uint8_t AC3BlkCod[] = {1, 2, 3, 6};
static const uint8_t AC3Channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
static const uint8_t DTSChannels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const uint8_t THDChanMap[] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

static const uint32_t DTSSampleRates[DTS_SFREQ_COUNT] = {0,     8000,  16000, 32000, 64000,  128000,
                                                         11025, 22050, 44100, 88200, 176400, 12000,
                                                         24000, 48000, 96000, 192000};

CAEStreamParser::CAEStreamParser() : m_syncFunc(&CAEStreamParser::DetectType)
{
  av_crc_init(m_crcTrueHD, 0, 16, 0x2D, sizeof(m_crcTrueHD));
}

double CAEStreamInfo::GetDuration() const
{
  double duration = 0;
  switch (m_type)
  {
    case STREAM_TYPE_AC3:
      duration = 1536.0 / m_sampleRate;
      break;
    case STREAM_TYPE_EAC3:
      duration = 6144.0 / m_sampleRate / 4;
      break;
    case STREAM_TYPE_TRUEHD:
      int rate;
      if (m_sampleRate == 48000 || m_sampleRate == 96000 || m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      duration = 3840.0 / rate;
      break;
    case STREAM_TYPE_DTSHD_MA:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_512:
    case STREAM_TYPE_DTSHD_CORE:
    case STREAM_TYPE_DTSHD:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_1024:
      duration = 1024.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_2048:
      duration = 2048.0 / m_sampleRate;
      break;
    default:
      CLog::Log(LOGERROR, "CAEStreamInfo::GetDuration - invalid stream type");
      break;
  }
  return duration * 1000;
}

bool CAEStreamInfo::operator==(const CAEStreamInfo& info) const
{
  if (m_type != info.m_type)
    return false;
  if (m_dataIsLE != info.m_dataIsLE)
    return false;
  if (m_repeat != info.m_repeat)
    return false;
  return true;
}

void CAEStreamParser::Reset()
{
  m_skipBytes = 0;
  m_bufferSize = 0;
  m_needBytes = 0;
  m_dtsChangeStreak = 0;
  m_dtsCandidateType = CAEStreamInfo::STREAM_TYPE_NULL;
  m_dtsCandidateRate = 0;
  m_dtsCandidateBlocks = 0;
  m_eac3ObjectsLatched = false;
  m_eac3ScanAttempts = 0;

  m_hasSync = false;
}

int CAEStreamParser::AddData(uint8_t* data,
                             unsigned int size,
                             uint8_t** buffer,
                             unsigned int* bufferSize)
{
  if (size == 0)
  {
    if (bufferSize)
      *bufferSize = 0;
    return 0;
  }

  if (m_skipBytes)
  {
    unsigned int canSkip = std::min(size, m_skipBytes);
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    unsigned int copy = std::min(room, canSkip);

    memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    m_skipBytes -= copy;

    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return copy;
    }

    GetPacket(buffer, bufferSize);
    return copy;
  }
  else
  {
    unsigned int consumed = 0;
    unsigned int offset = 0;
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    while (true)
    {
      if (!size)
      {
        if (bufferSize)
          *bufferSize = 0;
        return consumed;
      }

      unsigned int copy = std::min(room, size);
      memcpy(m_buffer + m_bufferSize, data, copy);
      m_bufferSize += copy;
      consumed += copy;
      data += copy;
      size -= copy;
      room -= copy;

      if (m_needBytes > m_bufferSize)
        continue;

      m_needBytes = 0;
      offset = (this->*m_syncFunc)(m_buffer, m_bufferSize);

      if (m_hasSync)
        break;
      else
      {
        // lost sync
        m_syncFunc = &CAEStreamParser::DetectType;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
        m_info.m_repeat = 1;

        // if the buffer is full, or the offset < the buffer size
        if (m_bufferSize == sizeof(m_buffer) || offset < m_bufferSize)
        {
          m_bufferSize -= offset;
          room += offset;
          memmove(m_buffer, m_buffer + offset, m_bufferSize);
        }
      }
    }

    // if we got here, we acquired sync on the buffer

    // align the buffer
    if (offset)
    {
      m_bufferSize -= offset;
      memmove(m_buffer, m_buffer + offset, m_bufferSize);
    }

    // bytes to skip until the next packet
    m_skipBytes = std::max(0, static_cast<int>(m_fsize) - static_cast<int>(m_bufferSize));
    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return consumed;
    }

    if (!m_needBytes)
      GetPacket(buffer, bufferSize);
    else if (bufferSize)
      *bufferSize = 0;

    return consumed;
  }
}

void CAEStreamParser::GetPacket(uint8_t** buffer, unsigned int* bufferSize)
{
  // if the caller wants the packet
  if (buffer)
  {
    // if it is dtsHD and we only want the core, just fetch that
    unsigned int size = m_fsize;
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE)
      size = m_coreSize;

    // Defeat AC-3/E-AC-3 dialnorm on the full frame before copying out
    if (m_defeatAC3DialNorm &&
        (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3))
    {
      DefeatAC3DialNorm(m_buffer, size);
    }

    if (m_defeatDTSDialNorm &&
        (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_512 ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_1024 ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTS_2048 ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE ||
         m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA))
    {
      DefeatDTSDialNorm(m_buffer, size);
    }

    // make sure the buffer is allocated and big enough
    if (!*buffer || !bufferSize || *bufferSize < size)
    {
      delete[] * buffer;
      *buffer = new uint8_t[size];
    }

    // copy the data into the buffer and update the size
    memcpy(*buffer, m_buffer, size);
    if (bufferSize)
      *bufferSize = size;
  }

  // remove the parsed data from the buffer
  m_bufferSize -= m_fsize;
  memmove(m_buffer, m_buffer + m_fsize, m_bufferSize);
  m_fsize = 0;
  m_coreSize = 0;
}

// SYNC FUNCTIONS

// This function looks for sync words across the types in parallel, and only does an exhaustive
// test if it finds a syncword. Once sync has been established, the relevant sync function sets
// m_syncFunc to itself. This function will only be called again if total sync is lost, which
// allows is to switch stream types on the fly much like a real receiver does.
unsigned int CAEStreamParser::DetectType(uint8_t* data, unsigned int size)
{
  unsigned int skipped = 0;
  unsigned int possible = 0;

  while (size > 8)
  {
    // DTS Sync Header check
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];

    // if it could be DTS
    if ((m_syncFamily == SyncFamily::Any || m_syncFamily == SyncFamily::DTS) &&
        (header == DTS_SYNC_CORE_14BE || header == DTS_SYNC_CORE_14LE ||
         header == DTS_SYNC_CORE_16BE || header == DTS_SYNC_CORE_16LE))
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be AC3
    if ((m_syncFamily == SyncFamily::Any || m_syncFamily == SyncFamily::AC3) &&
        data[0] == 0x0b && data[1] == 0x77)
    {
      unsigned int skip = SyncAC3(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be TrueHD
    if ((m_syncFamily == SyncFamily::Any || m_syncFamily == SyncFamily::TrueHD) &&
        data[4] == 0xf8 && data[5] == 0x72 && data[6] == 0x6f && data[7] == 0xba)
    {
      unsigned int skip = SyncTrueHD(data, size);
      if (m_hasSync)
        return skipped + skip;
      else
        possible = skipped;
    }

    // move along one byte
    --size;
    ++skipped;
    ++data;
  }

  return possible ? possible : skipped;
}

// ---------------------------------------------------------------------------
// AC-3 / E-AC-3 CRC-16 helpers for dialnorm defeat
// Reference: FFmpeg libavcodec/ac3enc.c (GPL-2.0-or-later)
// ---------------------------------------------------------------------------
#define AC3_CRC16_POLY ((1 << 0) | (1 << 2) | (1 << 15) | (1 << 16))

static inline uint16_t ac3_bswap16(uint16_t x) { return (x >> 8) | (x << 8); }

static unsigned int AC3_mul_poly(unsigned int a, unsigned int b, unsigned int poly)
{
  unsigned int c = 0;
  while (a)
  {
    if (a & 1) c ^= b;
    a >>= 1;
    b <<= 1;
    if (b & (1 << 16)) b ^= poly;
  }
  return c;
}

static unsigned int AC3_pow_poly(unsigned int a, unsigned int n, unsigned int poly)
{
  unsigned int r = 1;
  while (n)
  {
    if (n & 1) r = AC3_mul_poly(r, a, poly);
    a = AC3_mul_poly(a, a, poly);
    n >>= 1;
  }
  return r;
}

// Parse the 5-bit dialnorm field from an AC-3 BSI header.
// Returns the raw 5-bit value (0-31). The effective level is (raw - 31) dB,
// i.e. 0 = -31 dB, 1 = -30 dB, ..., 31 = 0 dB. Position depends on acmod.
static uint8_t AC3_ParseDialnorm(const uint8_t* data, uint8_t acmod)
{
  // Compute extra conditional bits between acmod and lfeon
  unsigned int extra = 0;
  if ((acmod & 0x1) && (acmod != 0x1)) extra += 2; // cmixlev
  if (acmod & 0x4) extra += 2;                     // surmixlev
  if (acmod == 0x2) extra += 2;                     // dsurmod

  // dialnorm starts at bit (4 + extra) from byte 6 MSB
  // Read bytes 6,7,8 as a 24-bit big-endian value
  uint32_t bits = (static_cast<uint32_t>(data[6]) << 16) |
                  (static_cast<uint32_t>(data[7]) << 8)  | data[8];
  unsigned int shift = 24 - (4 + extra) - 5; // = 15 - extra
  return (bits >> shift) & 0x1F;
}

// Set the 5-bit dialnorm field in an AC-3 BSI header to a given value.
static void AC3_SetDialnorm(uint8_t* data, uint8_t acmod, uint8_t value)
{
  unsigned int extra = 0;
  if ((acmod & 0x1) && (acmod != 0x1)) extra += 2;
  if (acmod & 0x4) extra += 2;
  if (acmod == 0x2) extra += 2;

  uint32_t bits = (static_cast<uint32_t>(data[6]) << 16) |
                  (static_cast<uint32_t>(data[7]) << 8)  | data[8];
  unsigned int shift = 15 - extra;
  bits = (bits & ~(0x1FU << shift)) | (static_cast<uint32_t>(value & 0x1F) << shift);
  data[6] = (bits >> 16) & 0xFF;
  data[7] = (bits >> 8) & 0xFF;
  data[8] = bits & 0xFF;
}

void CAEStreamParser::DefeatAC3DialNorm(uint8_t* data, unsigned int size)
{
  const AVCRC* crc_table = av_crc_get_table(AV_CRC_16_ANSI);
  unsigned int offset = 0;

  while (offset + 8 <= size)
  {
    // Each sub-frame must start with AC-3 sync word
    if (data[offset] != 0x0B || data[offset + 1] != 0x77) break;

    uint8_t* frame = data + offset;
    uint8_t bsid = frame[5] >> 3;
    unsigned int frame_bytes = 0;

    if (bsid <= 10)
    {
      // AC-3
      uint8_t fscod = frame[4] >> 6;
      uint8_t frmsizecod = frame[4] & 0x3F;
      if (fscod >= 3 || frmsizecod > 37) break;

      unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
      unsigned int framewords = 0;
      switch (fscod)
      {
        case 0: framewords = bitRate * 2; break;
        case 1: framewords = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0)); break;
        case 2: framewords = bitRate * 4; break;
      }
      frame_bytes = framewords * 2;
      if (offset + frame_bytes > size) break;

      uint8_t acmod = frame[6] >> 5;
      uint8_t dn = AC3_ParseDialnorm(frame, acmod);
      if (dn == 31)
      {
        // Already 0 dB (no normalization), nothing to defeat
        offset += frame_bytes;
        continue;
      }

      // Set dialnorm to 31 (= 0 dB, defeats normalization)
      AC3_SetDialnorm(frame, acmod, 31);

      // Recompute CRC-1 (covers first 5/8 of frame)
      unsigned int frame_size_58 = ((frame_bytes >> 2) + (frame_bytes >> 4)) << 1;
      uint16_t crc1 = ac3_bswap16(
        static_cast<uint16_t>(av_crc(crc_table, 0, frame + 4, frame_size_58 - 4)));
      unsigned int crc_inv = AC3_pow_poly((AC3_CRC16_POLY >> 1), (8 * frame_size_58) - 16, AC3_CRC16_POLY);
      crc1 = static_cast<uint16_t>(AC3_mul_poly(crc_inv, crc1, AC3_CRC16_POLY));
      frame[2] = (crc1 >> 8) & 0xFF;
      frame[3] = crc1 & 0xFF;

      // Recompute CRC-2 (covers second segment; first segment CRC is 0 so we start fresh)
      uint16_t crc2 = ac3_bswap16(
        static_cast<uint16_t>(av_crc(crc_table, 0, frame + frame_size_58, frame_bytes - frame_size_58 - 2)));
      if (crc2 == 0x0B77)
      {
        frame[frame_bytes - 3] ^= 0x1;
        crc2 ^= 0x8005;
      }
      frame[frame_bytes - 2] = (crc2 >> 8) & 0xFF;
      frame[frame_bytes - 1] = crc2 & 0xFF;
    }
    else if (bsid <= 16)
    {
      // E-AC-3
      uint8_t strmtyp = frame[2] >> 6;
      unsigned int framewords = (((frame[2] & 0x7) << 8) | frame[3]) + 1;
      frame_bytes = framewords * 2;
      if (offset + frame_bytes > size) break;

      if (strmtyp == 1)
      {
        offset += frame_bytes;
        continue;
      }

      // dialnorm: byte 5 bits[2:0] (MSBs) + byte 6 bits[7:6] (LSBs)
      uint8_t dn = ((frame[5] & 0x07) << 2) | ((frame[6] >> 6) & 0x03);
      if (dn == 31)
      {
        // Already 0 dB (no normalization), nothing to defeat
        offset += frame_bytes;
        continue;
      }

      // Set dialnorm to 31 (= 0 dB, no normalization)
      frame[5] |= 0x07;
      frame[6] |= 0xC0;

      // Recompute CRC-2 (covers bytes 2..frame_bytes-3)
      uint16_t crc2 = ac3_bswap16(
        static_cast<uint16_t>(av_crc(crc_table, 0, frame + 2, frame_bytes - 4)));
      if (crc2 == 0x0B77)
      {
        frame[frame_bytes - 3] ^= 0x1;
        crc2 ^= 0x8005;
      }
      frame[frame_bytes - 2] = (crc2 >> 8) & 0xFF;
      frame[frame_bytes - 1] = crc2 & 0xFF;
    }
    else break;

    offset += frame_bytes;
  }
}

static inline uint32_t DTS_ReadBits(const uint8_t* d, unsigned int& pos, unsigned int n)
{
  uint32_t v = 0;
  while (n--)
  {
    v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
    ++pos;
  }
  return v;
}

static inline void DTS_WriteBits(uint8_t* d, unsigned int pos, unsigned int n, uint32_t value)
{
  for (unsigned int i = 0; i < n; ++i)
  {
    const uint8_t mask = 1 << (7 - ((pos + i) & 7));
    if ((value >> (n - 1 - i)) & 1)
      d[(pos + i) >> 3] |= mask;
    else
      d[(pos + i) >> 3] &= ~mask;
  }
}

static inline unsigned int DTS_PopCount(uint32_t x)
{
  unsigned int c = 0;
  for (; x; x >>= 1)
    c += x & 1;
  return c;
}

static inline uint16_t DTS_CRC16_CCITT(const uint8_t* d, unsigned int len)
{
  uint16_t crc = 0xFFFF;
  for (unsigned int i = 0; i < len; ++i)
  {
    crc ^= static_cast<uint16_t>(d[i]) << 8;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}


namespace
{
struct DtsBitCursor
{
  const uint8_t* data;
  unsigned int pos;
  unsigned int limit;
  bool ok = true;

  DtsBitCursor(const uint8_t* buffer, unsigned int startBit, unsigned int limitBits)
    : data(buffer), pos(startBit), limit(limitBits)
  {
  }

  uint32_t Read(unsigned int n)
  {
    if (!ok || n > 32 || n > limit || pos > limit - n)
    {
      ok = false;
      return 0;
    }
    return DTS_ReadBits(data, pos, n);
  }

  void Skip(unsigned int n)
  {
    if (!ok || n > limit || pos > limit - n)
    {
      ok = false;
      return;
    }
    pos += n;
  }
};
}

static unsigned int DTS_SpeakerMaskHeightChannels(uint32_t mask)
{
  static const uint32_t heightBits[6] = {0x0020, 0x0080, 0x0100, 0x2000, 0x4000, 0x8000};
  static const unsigned int heightWidth[6] = {2, 1, 1, 2, 1, 2};

  unsigned int count = 0;
  for (unsigned int i = 0; i < 6; ++i)
    if (mask & heightBits[i])
      count += heightWidth[i];
  return count;
}

static bool DTS_ParseAssetChannels(const uint8_t* data,
                                   unsigned int size,
                                   unsigned int& channels,
                                   unsigned int& baseHeightChannels)
{
  channels = 0;
  baseHeightChannels = 0;
  if (size < 16)
    return false;

  const uint32_t coreSync = (static_cast<uint32_t>(data[0]) << 24) |
                            (static_cast<uint32_t>(data[1]) << 16) |
                            (static_cast<uint32_t>(data[2]) << 8) | data[3];
  if (coreSync != DTS_SYNC_CORE_16BE)
    return false;

  unsigned int pos = 32;
  DTS_ReadBits(data, pos, 1 + 5 + 1 + 7);
  const unsigned int fsize = DTS_ReadBits(data, pos, 14) + 1;
  if (fsize + 4 > size)
    return false;

  const uint32_t exssSync = (static_cast<uint32_t>(data[fsize]) << 24) |
                            (static_cast<uint32_t>(data[fsize + 1]) << 16) |
                            (static_cast<uint32_t>(data[fsize + 2]) << 8) | data[fsize + 3];
  if (exssSync != DTS_SYNC_EXTENTION)
    return false;

  const uint8_t* eb = data + fsize;
  const unsigned int available = size - fsize;
  DtsBitCursor bs(eb, 32, available * 8);

  bs.Read(8);
  const unsigned int nExtSSIndex = bs.Read(2);
  const unsigned int bHeaderSizeType = bs.Read(1);
  const unsigned int nBitsHdr = bHeaderSizeType ? 12 : 8;
  const unsigned int nBitsFsize = bHeaderSizeType ? 20 : 16;
  const unsigned int headerSize = bs.Read(nBitsHdr) + 1;
  if (!bs.ok || headerSize < 7 || headerSize > available)
    return false;
  if (DTS_CRC16_CCITT(eb + 5, headerSize - 5) != 0)
    return false;

  bs.limit = (headerSize - 2) * 8;

  bs.Read(nBitsFsize);
  if (!bs.Read(1))
    return false;

  bs.Read(2 + 3);
  if (bs.Read(1))
  {
    bs.Read(32);
    bs.Read(4);
  }
  const unsigned int numAudioPresnt = bs.Read(3) + 1;
  const unsigned int numAssets = bs.Read(3) + 1;
  if (numAudioPresnt > 8)
    return false;

  uint32_t masks[8] = {0};
  for (unsigned int i = 0; i < numAudioPresnt; ++i)
    masks[i] = bs.Read(nExtSSIndex + 1);
  for (unsigned int i = 0; i < numAudioPresnt; ++i)
    for (unsigned int ss = 0; ss < nExtSSIndex + 1u; ++ss)
      if ((masks[i] >> ss) & 1)
        bs.Read(8);

  if (bs.Read(1))
  {
    bs.Read(2);
    const unsigned int nuBits4MixOutMask = (bs.Read(2) + 1) << 2;
    const unsigned int numMixOutConfigs = bs.Read(2) + 1;
    for (unsigned int i = 0; i < numMixOutConfigs; ++i)
      bs.Read(nuBits4MixOutMask);
  }

  for (unsigned int i = 0; i < numAssets; ++i)
    bs.Read(nBitsFsize);

  const unsigned int descrStart = bs.pos;
  const unsigned int descrEnd = descrStart + (bs.Read(9) + 1) * 8;
  if (!bs.ok || descrEnd > bs.limit)
    return false;

  bs.Read(3);
  if (bs.Read(1))
    bs.Read(4);
  if (bs.Read(1))
    bs.Read(24);
  if (bs.Read(1))
    bs.Skip((bs.Read(10) + 1) * 8);
  bs.Read(5);
  bs.Read(4);
  const unsigned int totalChannels = bs.Read(8) + 1;

  if (!bs.ok || bs.pos > descrEnd || totalChannels > 32)
    return false;

  if (bs.Read(1))
  {
    if (totalChannels > 2)
      bs.Read(1);
    if (totalChannels > 6)
      bs.Read(1);
    if (bs.Read(1))
    {
      const unsigned int maskBits = (bs.Read(2) + 1) << 2;
      const uint32_t speakerMask = bs.Read(maskBits);
      if (bs.ok && bs.pos <= descrEnd)
        baseHeightChannels = DTS_SpeakerMaskHeightChannels(speakerMask);
    }
  }

  channels = totalChannels;
  return true;
}

void CAEStreamParser::DefeatDTSDialNorm(uint8_t* data, unsigned int size)
{
  if (size < 16)
    return;

  const uint32_t sync = (static_cast<uint32_t>(data[0]) << 24) |
                        (static_cast<uint32_t>(data[1]) << 16) |
                        (static_cast<uint32_t>(data[2]) << 8) | data[3];
  if (sync != DTS_SYNC_CORE_16BE)
    return;

  unsigned int pos = 32;
  DTS_ReadBits(data, pos, 1);
  DTS_ReadBits(data, pos, 5);
  const unsigned int cpf = DTS_ReadBits(data, pos, 1);
  DTS_ReadBits(data, pos, 7);
  const unsigned int fsize = DTS_ReadBits(data, pos, 14) + 1;
  DTS_ReadBits(data, pos, 6 + 4 + 5);
  DTS_ReadBits(data, pos, 1 + 1 + 1 + 1 + 1);
  DTS_ReadBits(data, pos, 3 + 1 + 1 + 2 + 1);
  if (cpf)
    DTS_ReadBits(data, pos, 16);
  DTS_ReadBits(data, pos, 1 + 4 + 2 + 3 + 1 + 1);
  const unsigned int dngPos = pos;
  const unsigned int dng = DTS_ReadBits(data, pos, 4);

  if (cpf == 0 && dng != 0)
    DTS_WriteBits(data, dngPos, 4, 0);

  if (fsize + 4 > size)
    return;
  const unsigned int exss = fsize;
  const uint32_t exssSync = (static_cast<uint32_t>(data[exss]) << 24) |
                            (static_cast<uint32_t>(data[exss + 1]) << 16) |
                            (static_cast<uint32_t>(data[exss + 2]) << 8) | data[exss + 3];
  if (exssSync != DTS_SYNC_EXTENTION)
    return;

  uint8_t* eb = data + exss;
  unsigned int ep = 32;
  DTS_ReadBits(eb, ep, 8);
  const unsigned int nExtSSIndex = DTS_ReadBits(eb, ep, 2);
  const unsigned int bHeaderSizeType = DTS_ReadBits(eb, ep, 1);
  const unsigned int nBitsHdr = bHeaderSizeType ? 12 : 8;
  const unsigned int nBitsFsize = bHeaderSizeType ? 20 : 16;
  const unsigned int headerSize = DTS_ReadBits(eb, ep, nBitsHdr) + 1;
  if (exss + headerSize > size || headerSize < 7)
    return;

  if (DTS_CRC16_CCITT(eb + 5, headerSize - 5) != 0)
    return;

  DTS_ReadBits(eb, ep, nBitsFsize);
  const unsigned int staticFields = DTS_ReadBits(eb, ep, 1);
  unsigned int numAssets = 1;
  if (staticFields)
  {
    DTS_ReadBits(eb, ep, 2 + 3);
    if (DTS_ReadBits(eb, ep, 1))
    {
      DTS_ReadBits(eb, ep, 32);
      DTS_ReadBits(eb, ep, 4);
    }
    const unsigned int numAudioPresnt = DTS_ReadBits(eb, ep, 3) + 1;
    numAssets = DTS_ReadBits(eb, ep, 3) + 1;
    if (numAudioPresnt > 8)
      return;
    uint32_t masks[8] = {0};
    for (unsigned int i = 0; i < numAudioPresnt; ++i)
      masks[i] = DTS_ReadBits(eb, ep, nExtSSIndex + 1);
    for (unsigned int i = 0; i < numAudioPresnt; ++i)
      for (unsigned int ss = 0; ss < nExtSSIndex + 1u; ++ss)
        if ((masks[i] >> ss) & 1)
          DTS_ReadBits(eb, ep, 8);
    if (DTS_ReadBits(eb, ep, 1))
      return;
  }
  for (unsigned int i = 0; i < numAssets; ++i)
    DTS_ReadBits(eb, ep, nBitsFsize);

  const unsigned int descrStart = ep;
  const unsigned int descrEnd = descrStart + (DTS_ReadBits(eb, ep, 9) + 1) * 8;
  if (descrEnd > (headerSize - 2) * 8)
    return;
  DTS_ReadBits(eb, ep, 3);
  if (staticFields)
  {
    if (DTS_ReadBits(eb, ep, 1)) DTS_ReadBits(eb, ep, 4);
    if (DTS_ReadBits(eb, ep, 1)) DTS_ReadBits(eb, ep, 24);
    if (DTS_ReadBits(eb, ep, 1))
      ep += (DTS_ReadBits(eb, ep, 10) + 1) * 8;
    DTS_ReadBits(eb, ep, 5);
    DTS_ReadBits(eb, ep, 4);
    const unsigned int nch = DTS_ReadBits(eb, ep, 8) + 1;
    if (DTS_ReadBits(eb, ep, 1))
    {
      if (nch > 2) DTS_ReadBits(eb, ep, 1);
      if (nch > 6) DTS_ReadBits(eb, ep, 1);
      unsigned int w = 0;
      if (DTS_ReadBits(eb, ep, 1))
      {
        w = (DTS_ReadBits(eb, ep, 2) + 1) * 4;
        DTS_ReadBits(eb, ep, w);
      }
      const unsigned int nremap = DTS_ReadBits(eb, ep, 3);
      if (nremap > 8)
        return;
      uint32_t layouts[8] = {0};
      for (unsigned int i = 0; i < nremap; ++i)
        layouts[i] = DTS_ReadBits(eb, ep, w);
      for (unsigned int i = 0; i < nremap; ++i)
      {
        const unsigned int ndec = DTS_ReadBits(eb, ep, 5) + 1;
        const unsigned int nspk = DTS_PopCount(layouts[i]);
        for (unsigned int c = 0; c < nspk; ++c)
        {
          const unsigned int ncoef = DTS_PopCount(DTS_ReadBits(eb, ep, ndec));
          for (unsigned int k = 0; k < ncoef; ++k)
            DTS_ReadBits(eb, ep, 5);
        }
      }
    }
    else
      DTS_ReadBits(eb, ep, 3);
  }
  if (DTS_ReadBits(eb, ep, 1))
    DTS_ReadBits(eb, ep, 8);
  if (!DTS_ReadBits(eb, ep, 1))
    return;
  const unsigned int dnPos = ep;
  if (dnPos + 5 > descrEnd)
    return;
  if (DTS_ReadBits(eb, ep, 5) == 0)
    return;

  DTS_WriteBits(eb, dnPos, 5, 0);
  const uint16_t crc = DTS_CRC16_CCITT(eb + 5, headerSize - 2 - 5);
  eb[headerSize - 2] = (crc >> 8) & 0xFF;
  eb[headerSize - 1] = crc & 0xFF;
}

namespace
{
struct AtmosBitReader
{
  const uint8_t* data;
  unsigned int pos;
  unsigned int limit;
  bool ok = true;

  AtmosBitReader(const uint8_t* buffer, unsigned int startBit, unsigned int endBit)
    : data(buffer), pos(startBit), limit(endBit)
  {
  }

  AtmosBitReader(const uint8_t* buffer,
                 unsigned int startBit,
                 unsigned int endBit,
                 unsigned int sizeBytes)
    : data(buffer), pos(startBit), limit(std::min(endBit, sizeBytes * 8))
  {
  }

  bool Skip(unsigned int n)
  {
    if (!ok || n > limit || pos > limit - n)
    {
      ok = false;
      return false;
    }
    pos += n;
    return true;
  }

  uint32_t Read(unsigned int n)
  {
    if (n > 32 || !Skip(n))
    {
      ok = false;
      return 0;
    }

    uint32_t v = 0;
    for (unsigned int bit = pos - n; bit < pos; ++bit)
      v = (v << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
    return v;
  }
};

struct AtmosProgram
{
  int bedChannels = -1;
  int declaredObjects = -1;
  bool objectOnlyProgram = false;
};

constexpr uint8_t ATMOS_STD_BED_WIDTH[10] = {2, 1, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr uint8_t ATMOS_ISF_OBJECTS[6] = {4, 8, 10, 14, 15, 30};
constexpr uint8_t ATMOS_TRUEHD_ISF_OBJECTS[8] = {0, 0, 10, 14, 15, 0, 0, 0};

enum class AtmosCarriage
{
  EAC3,
  TRUEHD
};
constexpr unsigned int ATMOS_EMDF_SYNCWORD = 0x5838;

AtmosProgram Atmos_ParseProgramAssignment(AtmosBitReader& br,
                                          int elementCount,
                                          AtmosCarriage carriage)
{
  AtmosProgram prog;

  if (br.Read(1))
  {
    const bool lfePresent = br.Read(1) != 0;
    if (!br.ok)
      return prog;

    prog.bedChannels = lfePresent ? 1 : 0;
    prog.objectOnlyProgram = true;
    if (elementCount > 0)
      prog.declaredObjects = std::max(0, elementCount - prog.bedChannels);
    return prog;
  }

  const uint32_t contentDescription = br.Read(4);
  if (!br.ok)
    return prog;
  int bedChannels = 0;

  if (contentDescription & 0x1)
  {
    br.Read(1);
    uint32_t bedInstances = 1;
    if (br.Read(1))
      bedInstances = br.Read(3) + 2;

    for (uint32_t bed = 0; bed < bedInstances && br.ok; ++bed)
    {
      if (br.Read(1))
      {
        ++bedChannels;
        continue;
      }

      if (br.Read(1))
      {
        for (unsigned int i = 0; i < 10; ++i)
          if (br.Read(1))
            bedChannels += ATMOS_STD_BED_WIDTH[9 - i];
      }
      else
      {
        for (unsigned int i = 0; i < 17; ++i)
          if (br.Read(1))
            ++bedChannels;
      }
    }
  }

  int isfObjects = 0;
  if (contentDescription & 0x2)
  {
    const uint32_t isfIndex = br.Read(3);
    if (!br.ok)
      return prog;
    if (carriage == AtmosCarriage::TRUEHD)
    {
      if (ATMOS_TRUEHD_ISF_OBJECTS[isfIndex] == 0)
        return prog;
      isfObjects = ATMOS_TRUEHD_ISF_OBJECTS[isfIndex];
    }
    else if (isfIndex < 6)
    {
      isfObjects = ATMOS_ISF_OBJECTS[isfIndex];
    }
  }

  int declared = 0;
  if (contentDescription & 0x4)
  {
    uint32_t objectBits = br.Read(5);
    if (objectBits == 0x1F)
      objectBits += br.Read(7);
    declared = static_cast<int>(objectBits) + 1;
    if (elementCount > 0 && declared + isfObjects + bedChannels > elementCount)
      return prog;
  }

  if (contentDescription & 0x8)
  {
    const uint32_t reservedBytes = br.Read(4) + 1;
    br.Skip(reservedBytes * 8);
  }

  if (!br.ok)
    return prog;

  prog.bedChannels = bedChannels;
  prog.declaredObjects = declared;
  return prog;
}

int Atmos_DerivedObjects(const AtmosProgram& prog, int elementCount)
{
  if (prog.bedChannels < 0 || elementCount <= 0)
    return -1;

  return std::max(0, elementCount - prog.bedChannels);
}

uint32_t Atmos_VariableBits(AtmosBitReader& br, unsigned int n)
{
  uint32_t value = 0;
  for (unsigned int guard = 0; guard < 8 && br.ok; ++guard)
  {
    value += br.Read(n);
    if (!br.Read(1))
      return value;
    value <<= n;
    value += (1u << n);
  }

  br.ok = false;
  return 0;
}

bool Atmos_SkipEmdfPayloadConfig(AtmosBitReader& br)
{
  const bool smpOffsetPresent = br.Read(1) != 0;
  if (smpOffsetPresent)
    br.Skip(12);

  if (br.Read(1))
    Atmos_VariableBits(br, 11);
  if (br.Read(1))
    Atmos_VariableBits(br, 2);
  if (br.Read(1))
    br.Skip(8);

  if (!br.Read(1))
  {
    bool frameAligned = false;
    if (!smpOffsetPresent)
    {
      frameAligned = br.Read(1) != 0;
      if (frameAligned)
        br.Skip(2);
    }
    if (smpOffsetPresent || frameAligned)
      br.Skip(7);
  }

  return br.ok;
}

bool Atmos_ParseOamd(AtmosBitReader& br, AtmosProgram& prog, int& elementCount)
{
  uint32_t version = br.Read(2);
  if (version == 3)
    version += br.Read(3);
  if (!br.ok || version != 0)
    return false;

  int count = static_cast<int>(br.Read(5)) + 1;
  if (count == 32)
    count += static_cast<int>(br.Read(7));
  if (!br.ok || count <= 0)
    return false;

  const AtmosProgram parsed =
      Atmos_ParseProgramAssignment(br, count, AtmosCarriage::EAC3);
  if (parsed.declaredObjects < 0 || parsed.bedChannels < 0)
    return false;

  prog = parsed;
  elementCount = count;
  return true;
}

bool Atmos_TryEmdfContainerAt(const uint8_t* data,
                              unsigned int syncBit,
                              unsigned int limitBit,
                              AtmosProgram& prog,
                              int& elementCount)
{
  AtmosBitReader br{data, syncBit, limitBit};

  br.Skip(16);
  const uint32_t containerLength = br.Read(16);
  if (!br.ok)
    return false;

  const uint64_t containerEnd64 = static_cast<uint64_t>(br.pos) + containerLength * 8ull;
  if (containerEnd64 > limitBit)
    return false;
  const unsigned int containerEnd = static_cast<unsigned int>(containerEnd64);

  if (br.Read(2) != 0)
    return false;
  if (br.Read(3) == 0x7)
    Atmos_VariableBits(br, 3);

  bool found = false;
  AtmosProgram foundProg;
  int foundElements = -1;

  for (unsigned int payload = 0; payload < 32; ++payload)
  {
    uint32_t payloadId = br.Read(5);
    if (payloadId == 0x1F)
      payloadId += Atmos_VariableBits(br, 5);
    if (!br.ok)
      break;

    if (payloadId == 0)
      break;

    if (!Atmos_SkipEmdfPayloadConfig(br))
      break;

    const uint64_t payloadBits = static_cast<uint64_t>(Atmos_VariableBits(br, 8)) * 8ull;
    const unsigned int payloadStart = br.pos;
    if (!br.ok || payloadStart > containerEnd ||
        payloadBits > static_cast<uint64_t>(containerEnd - payloadStart))
      break;

    const unsigned int payloadEnd = payloadStart + static_cast<unsigned int>(payloadBits);
    if (payloadId == 11 && !found)
    {
      AtmosBitReader payloadReader{data, payloadStart, std::min(payloadEnd, limitBit)};
      found = Atmos_ParseOamd(payloadReader, foundProg, foundElements);
    }

    br.pos = payloadStart;
    if (!br.Skip(static_cast<unsigned int>(payloadBits)))
      break;
  }

  if (found)
  {
    prog = foundProg;
    elementCount = foundElements;
  }
  return found;
}

bool Atmos_ScanWindowForOamd(const uint8_t* data,
                             unsigned int startBit,
                             unsigned int endBit,
                             AtmosProgram& prog,
                             int& elementCount)
{
  if (endBit < 16 || startBit > endBit - 16)
    return false;

  for (unsigned int bit = startBit; bit <= endBit - 16; ++bit)
  {
    if (((data[bit >> 3] >> (7 - (bit & 7))) & 1) != 0)
      continue;

    AtmosBitReader peek{data, bit, endBit};
    if (peek.Read(16) != ATMOS_EMDF_SYNCWORD)
      continue;

    if (Atmos_TryEmdfContainerAt(data, bit, endBit, prog, elementCount))
      return true;
  }

  return false;
}

bool Atmos_ParseEAC3Objects(const uint8_t* data,
                            unsigned int frameBytes,
                            AtmosProgram& prog,
                            int& elementCount)
{
  const unsigned int frameBits = frameBytes * 8;
  if (frameBits < 32)
    return false;

  const unsigned int trailerStart = frameBits - 32;
  AtmosBitReader footer{data, trailerStart, frameBits};
  const unsigned int auxLength = footer.Read(14);
  const bool auxPresent = footer.Read(1) != 0;

  if (auxPresent && footer.ok)
  {
    const unsigned int auxBits = auxLength;
    if (auxBits <= trailerStart &&
        Atmos_ScanWindowForOamd(data, trailerStart - auxBits, trailerStart, prog, elementCount))
      return true;
  }

  return Atmos_ScanWindowForOamd(data, 0, frameBits, prog, elementCount);
}
}

bool CAEStreamParser::TrySyncAC3(uint8_t* data,
                                 unsigned int size,
                                 bool resyncing,
                                 bool wantEAC3dependent)
{

  // https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf

  if (size < 8)
    return false;

  // look for an ac3 sync word
  if (data[0] != 0x0b || data[1] != 0x77)
    return false;

  uint8_t bsid = data[5] >> 3;
  uint8_t acmod = data[6] >> 5;
  uint8_t lfeon;

  int8_t pos = 4;
  if ((acmod & 0x1) && (acmod != 0x1))
    pos -= 2;
  if (acmod & 0x4)
    pos -= 2;
  if (acmod == 0x2)
    pos -= 2;
  if (pos < 0)
    lfeon = (data[7] & 0x64) ? 1 : 0;
  else
    lfeon = ((data[6] >> pos) & 0x1) ? 1 : 0;

  if (bsid > 0x11 || acmod > 7)
    return false;

  if (bsid <= 10)
  {
    // Normal AC-3

    if (wantEAC3dependent)
      return false;

    uint8_t fscod = data[4] >> 6;
    uint8_t frmsizecod = data[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
      return false;

    // get the details we need to check crc1 and framesize
    unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch (fscod)
    {
      case 0:
        framesize = bitRate * 2;
        break;
      case 1:
        framesize = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
        break;
      case 2:
        framesize = bitRate * 4;
        break;
    }

    m_fsize = framesize << 1;
    m_info.m_sampleRate = AC3FSCod[fscod];

    // Parse dialnorm for logging (available in first 8 bytes)
    // 5-bit raw value 0-31 maps to -31 to 0 dB (dB = raw - 31)
    {
      uint8_t ac3_acmod = data[6] >> 5;
      uint8_t dialNormRaw = AC3_ParseDialnorm(data, ac3_acmod);
      m_info.m_dialNorm = static_cast<int>(dialNormRaw) - 31;
      m_info.m_dialNormApplied = m_defeatAC3DialNorm ? 0 : m_info.m_dialNorm;
      m_info.m_hasDialNorm = true;
    }

    // dont do extensive testing if we have not lost sync
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !resyncing)
      return true;

    // this may be the main stream of EAC3
    unsigned int fsizeMain = m_fsize;
    unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - AC3 Not enough data for frame");
      // not enough data to check for AC3 frame, request more
      m_needBytes = reqBytes;
      m_fsize = 0;
      // no need to resync => return true
      return true;
    }
    m_info.m_ac3FrameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      // concatenate the main and dependent frames
      m_fsize += fsizeMain;
      return true;
    }

    unsigned int crc_size;
    // if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet)
    if (framesize <= size)
      crc_size = framesize - 1;
    else
      crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= size)
      if (av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, &data[2], crc_size * 2))
        return false;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    m_info.m_ac3FrameSize += m_fsize;
    m_info.m_repeat = 1;
    m_info.m_bitDepth = 16;

    {
      uint8_t ac3_acmod = data[6] >> 5;
      uint8_t dialNormRaw = AC3_ParseDialnorm(data, ac3_acmod);
      m_info.m_dialNorm = static_cast<int>(dialNormRaw) - 31;
      m_info.m_dialNormApplied = m_defeatAC3DialNorm ? 0 : m_info.m_dialNorm;
      m_info.m_hasDialNorm = true;
    }
    {
      const std::string msg = fmt::format(
          "CAEStreamParser::TrySyncAC3 - AC3 stream detected ({} channels, {}Hz, "
          "dialnorm: {} dB{})",
          m_info.m_channels, m_info.m_sampleRate, m_info.m_dialNorm,
          m_defeatAC3DialNorm ? ", defeat enabled" : "");
      if (msg != m_lastLoggedStreamDetected)
      {
        CLog::Log(LOGINFO, "{}", msg);
        m_lastLoggedStreamDetected = msg;
      }
    }
    return true;
  }
  else
  {
    // Enhanced AC-3
    uint8_t strmtyp = data[2] >> 6;
    if (strmtyp == 3)
      return false;

    if (strmtyp != 1 && wantEAC3dependent)
      return false;

    unsigned int framesize = (((data[2] & 0x7) << 8) | data[3]) + 1;

    uint8_t fscod = (data[4] >> 6) & 0x3;
    uint8_t cod = (data[4] >> 4) & 0x3;  // numblkscod ?
    uint8_t acmod = (data[4] >> 1) & 0x7;
    uint8_t lfeon = data[4] & 0x1;
    uint8_t blocks;

    if (fscod == 0x3)
    {
      if (cod == 0x3)
        return false;

      blocks = 6;
      m_info.m_sampleRate = AC3FSCod[cod] >> 1;
    }
    else
    {
      blocks = AC3BlkCod[cod];
      m_info.m_sampleRate = AC3FSCod[fscod];
    }

    m_fsize = framesize << 1; // Convert Frame size to bytes (<<1 is multiply by 2)
    m_info.m_repeat = MAX_EAC3_BLOCKS / blocks;

    // Parse dialnorm for logging
    // 5-bit raw value 0-31 maps to -31 to 0 dB (dB = raw - 31)
    {
      uint8_t dialNormRaw = ((data[5] & 0x07) << 2) | ((data[6] >> 6) & 0x03);
      m_info.m_dialNorm = static_cast<int>(dialNormRaw) - 31;
      m_info.m_dialNormApplied = m_defeatAC3DialNorm ? 0 : m_info.m_dialNorm;
      m_info.m_hasDialNorm = true;
    }

    const bool frameCrcValid =
        m_fsize > 2 && size >= m_fsize &&
        av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, &data[2], m_fsize - 2) == 0;

    if (!m_eac3ObjectsLatched && m_fsize > 2 && size >= m_fsize && !frameCrcValid)
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAUDIO, 1000,
                            "CAEStreamParser::TrySyncAC3 - frameCrcValid={:d} "
                            "m_eac3ObjectsLatched={:d} m_fsize={} size={}",
                            frameCrcValid, m_eac3ObjectsLatched, m_fsize, size);

    if (!m_eac3ObjectsLatched && m_eac3ScanAttempts < EAC3ObjectScanBudget() && m_fsize > 0 &&
        size >= m_fsize && frameCrcValid)
    {
      ++m_eac3ScanAttempts;

      AtmosProgram prog;
      int elementCount = -1;
      if (Atmos_ParseEAC3Objects(data, m_fsize, prog, elementCount))
      {
        const int derived = Atmos_DerivedObjects(prog, elementCount);
        m_info.m_hasAtmos = true;
        m_info.m_atmosChannels = static_cast<unsigned int>(elementCount);
        m_info.m_atmosObjects = prog.declaredObjects;
        m_info.m_bedChannels = prog.objectOnlyProgram ? 0 : prog.bedChannels;
        m_info.m_bedIsLfeOnly = prog.objectOnlyProgram && prog.bedChannels == 1;
        m_eac3ObjectsLatched = true;
        logM(LOGINFO,
             "CAEStreamParser::TrySyncAC3 - E-AC3 JOC object metadata found after {} scans "
                  "(atmosChannels: {}, atmosObjects: {}, derivedObjects: {})",
                  m_eac3ScanAttempts, elementCount, prog.declaredObjects, derived);
      }
      else if (m_eac3ScanAttempts >= EAC3ObjectScanBudget())
      {
        m_eac3ObjectsLatched = true;
        logM(LOGINFO,
             "CAEStreamParser::TrySyncAC3 - E-AC3 no object metadata in {} scans, giving up "
                  "for this stream{}",
                  m_eac3ScanAttempts, m_eac3IsJOC ? " (demuxer typed it Atmos)" : "");
      }
    }

    // EAC3 can have a dependent stream too
    if (!wantEAC3dependent)
    {
      unsigned int fsizeMain = m_fsize;
      unsigned int reqBytes = fsizeMain + 8;

      if (size < reqBytes)
      {
        CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - E-AC3 Not enough data for frame");
        // not enough data to check for E-AC3 frame, request more
        m_needBytes = reqBytes;
        m_fsize = 0;
        // no need to resync => return true
        return true;
      }

      m_info.m_ac3FrameSize = fsizeMain;
      if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
      {
        // concatenate the main and dependent frames
        m_fsize += fsizeMain;
        return true;
      }
    }

    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && m_hasSync && !resyncing)
      return true;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    m_info.m_ac3FrameSize += m_fsize;
    m_info.m_bitDepth = 16;

    {
      uint8_t dialNormRaw = ((data[5] & 0x07) << 2) | ((data[6] >> 6) & 0x03);
      m_info.m_dialNorm = static_cast<int>(dialNormRaw) - 31;
      m_info.m_dialNormApplied = m_defeatAC3DialNorm ? 0 : m_info.m_dialNorm;
      m_info.m_hasDialNorm = true;
    }
    {
      const std::string msg = fmt::format(
          "CAEStreamParser::TrySyncAC3 - E-AC3 stream detected ({} channels, {}Hz, {}-bit, "
          "dialnorm: {} dB{})",
          m_info.m_channels, m_info.m_sampleRate, m_info.m_bitDepth,
          m_info.m_dialNorm, m_defeatAC3DialNorm ? ", defeat enabled" : "");
      if (msg != m_lastLoggedStreamDetected)
      {
        CLog::Log(LOGINFO, "{}", msg);
        m_lastLoggedStreamDetected = msg;
      }
    }

    return true;
  }
}

unsigned int CAEStreamParser::SyncAC3(uint8_t* data, unsigned int size)
{
  unsigned int skip = 0;

  for (; size - skip > 7; ++skip, ++data)
  {
    bool resyncing = (skip != 0);
    if (TrySyncAC3(data, size - skip, resyncing, false))
      return skip;
  }

  // if we get here, the entire packet is invalid and we have lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncAC3 - AC3 sync lost");
  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::SyncDTS(uint8_t* data, unsigned int size)
{
  if (size < 13)
  {
    if (m_needBytes < 13)
      m_needBytes = 14;
    return 0;
  }

  unsigned int skip = 0;
  for (; size - skip > 13; ++skip, ++data)
  {
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    unsigned int dtsBlocks;
    unsigned int amode;
    unsigned int sfreq;
    unsigned int target_rate;
    unsigned int extension = 0;
    unsigned int ext_type = UNKNOWN_DTS_EXTENSION;
    unsigned int lfe;
    uint32_t bits = 0;
    bool dataIsLE = m_info.m_dataIsLE;

    switch (header)
    {
      // 14bit BE
      case DTS_SYNC_CORE_14BE:
        if (data[4] != 0x07 || (data[5] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[5] & 0x7) << 4) | ((data[6] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[6] & 0x3) << 8) | data[7]) << 4) | ((data[8] & 0x3C) >> 2)) + 1;
        amode = ((data[8] & 0x3) << 4) | ((data[9] & 0xF0) >> 4);
        target_rate = ((data[10] & 0x3e) >> 1);
        extension = ((data[11] & 0x1));
        ext_type = ((data[11] & 0xe) >> 1);
        sfreq = data[9] & 0xF;
        lfe = (data[12] & 0x18) >> 3;
        dataIsLE = false;
        bits = 14;
        break;

      // 14bit LE
      case DTS_SYNC_CORE_14LE:
        if (data[5] != 0x07 || (data[4] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[4] & 0x7) << 4) | ((data[7] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[7] & 0x3) << 8) | data[6]) << 4) | ((data[9] & 0x3C) >> 2)) + 1;
        amode = ((data[9] & 0x3) << 4) | ((data[8] & 0xF0) >> 4);
        target_rate = ((data[11] & 0x3e) >> 1);
        extension = ((data[10] & 0x1));
        ext_type = ((data[10] & 0xe) >> 1);
        sfreq = data[8] & 0xF;
        lfe = (data[13] & 0x18) >> 3;
        dataIsLE = true;
        bits = 14;
        break;

      // 16bit BE
      case DTS_SYNC_CORE_16BE:
        dtsBlocks = (((data[4] & 0x1) << 7) | ((data[5] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[5] & 0x3) << 8) | data[6]) << 4) | ((data[7] & 0xF0) >> 4)) + 1;
        amode = ((data[7] & 0x0F) << 2) | ((data[8] & 0xC0) >> 6);
        sfreq = (data[8] & 0x3C) >> 2;
        target_rate = ((data[8] & 0x03) << 3) | ((data[9] & 0xe0) >> 5);
        extension = (data[10] & 0x10) >> 4;
        ext_type = (data[10] & 0xe0) >> 5;
        lfe = (data[10] >> 1) & 0x3;
        dataIsLE = false;
        bits = 16;
        break;

      // 16bit LE
      case DTS_SYNC_CORE_16LE:
        dtsBlocks = (((data[5] & 0x1) << 7) | ((data[4] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[4] & 0x3) << 8) | data[7]) << 4) | ((data[6] & 0xF0) >> 4)) + 1;
        amode = ((data[6] & 0x0F) << 2) | ((data[9] & 0xC0) >> 6);
        sfreq = (data[9] & 0x3C) >> 2;
        target_rate = ((data[9] & 0x03) << 3) | ((data[8] & 0xe0) >> 5);
        extension = (data[11] & 0x10) >> 4;
        ext_type = (data[11] & 0xe0) >> 5;
        lfe = (data[11] >> 1) & 0x3;
        dataIsLE = true;
        bits = 16;
        break;

      default:
        continue;
    }

    if (sfreq == 0 || sfreq >= DTS_SFREQ_COUNT)
      continue;

    if (amode >= sizeof(DTSChannels) / sizeof(DTSChannels[0]))
      continue;

    // make sure the framesize is sane
    if (m_fsize < 96 || m_fsize > 16384)
      continue;

    CAEStreamInfo::DataType dataType{CAEStreamInfo::STREAM_TYPE_NULL};
    switch (dtsBlocks << 5)
    {
      case 512:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_512;
        break;
      case 1024:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_1024;
        break;
      case 2048:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_2048;
        break;
    }

    if (dataType == CAEStreamInfo::STREAM_TYPE_NULL)
      continue;

    // adjust the fsize for 14 bit streams
    if (bits == 14)
      m_fsize = m_fsize / 14 * 16;

    // we need enough data to check for DTS-HD
    if (size - skip < m_fsize + 10)
    {
      // we can assume DTS sync at this point
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_needBytes = m_fsize + 10;
      m_fsize = 0;

      return skip;
    }

    // Check for a Stream Extention after the core frame.
    uint32_t ext_sync = (data[m_fsize] << 24) | (data[m_fsize + 1] << 16) | (data[m_fsize + 2] << 8) | data[m_fsize + 3];
    uint32_t ext_sub_sync = 0;
    uint32_t ext_header_size = 0;

    // Have a Stream Extention.
    if (ext_sync == DTS_SYNC_EXTENTION)
    {
      uint32_t ext_size;
      bool blownup = (data[m_fsize + 5] & 0x20) != 0;
      if (blownup)
        ext_size = (((data[m_fsize + 6] & 0x01) << 19) | (data[m_fsize + 7] << 11) |
                   (data[m_fsize + 8] << 3) | ((data[m_fsize + 9] & 0xe0) >> 5)) +
                  1;
      else
        ext_size = (((data[m_fsize + 6] & 0x1f) << 11) | (data[m_fsize + 7] << 3) |
                   ((data[m_fsize + 8] & 0xe0) >> 5)) +
                  1;

      if (blownup)
        ext_header_size = (((data[m_fsize + 5] & 0x1f) << 7) | ((data[m_fsize + 6] & 0xfe) >> 1)) + 1;
      else
        ext_header_size = (((data[m_fsize + 5] & 0x1f) << 3) | ((data[m_fsize + 6] & 0xe0) >> 5)) + 1;

      if (size - skip < m_fsize + ext_header_size + 4)
      {
        m_syncFunc = &CAEStreamParser::SyncDTS;
        m_needBytes = m_fsize + ext_header_size + 4;
        m_fsize = 0;

        return skip;
      }

      ext_sub_sync = data[m_fsize + ext_header_size] << 24 | data[m_fsize + ext_header_size + 1] << 16 |
                     data[m_fsize + ext_header_size + 2] << 8 | data[m_fsize + ext_header_size + 3];

      // set the type according to core or not
      if (m_coreOnly)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
      else if (ext_sub_sync == DTS_SYNC_EXT_XLL)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
      else if (ext_sub_sync == DTS_SYNC_EXT_XCH ||  ext_sub_sync == DTS_SYNC_EXT_XXCH ||
               ext_sub_sync == DTS_SYNC_EXT_X96K || ext_sub_sync == DTS_SYNC_EXT_XBR ||
               ext_sub_sync == DTS_SYNC_EXT_LBR)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD;
      else
      {
        if (m_info.m_type != CAEStreamInfo::STREAM_TYPE_NULL)
          dataType = m_info.m_type;
      }

      m_coreSize = m_fsize;
      m_fsize += ext_size;
    }

    unsigned int sampleRate = DTSSampleRates[sfreq];

    const bool paramsChanged = dataType != m_info.m_type ||
                               sampleRate != m_info.m_sampleRate || dtsBlocks != m_dtsBlocks ||
                               dataIsLE != m_info.m_dataIsLE;

    if (!m_hasSync || skip || paramsChanged)
    {
      if (m_hasSync && paramsChanged && m_info.m_type != CAEStreamInfo::STREAM_TYPE_NULL)
      {
        unsigned int candidatePeriod;
        if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
          candidatePeriod = (192000 * (8 >> 1)) * (dtsBlocks << 5) / sampleRate;
        else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
          candidatePeriod = (192000 * (2 >> 1)) * (dtsBlocks << 5) / sampleRate;
        else
          candidatePeriod = (sampleRate * (2 >> 1)) * (dtsBlocks << 5) / sampleRate;

        if (ext_sync != DTS_SYNC_EXTENTION)
          m_coreSize = m_fsize;

        const bool wrapped = dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
                             dataType == CAEStreamInfo::STREAM_TYPE_DTSHD;

        if (wrapped && !IsPackableDtsBurst(candidatePeriod))
        {
          LOG_THROTTLE_PERIODIC_GENERAL(
              LOGINFO, 1000,
              "CAEStreamParser::SyncDTS - rejecting DTS reclassification: period {} has no IEC "
              "61937 burst subtype, keeping established format",
              candidatePeriod);
          return skip;
        }

        unsigned int frameBytes;
        if (wrapped)
          frameBytes = (candidatePeriod << 2);
        else if (dataType == CAEStreamInfo::STREAM_TYPE_DTS_1024)
          frameBytes = OUT_FRAMESTOBYTES(DTS2_FRAME_SIZE);
        else if (dataType == CAEStreamInfo::STREAM_TYPE_DTS_2048)
          frameBytes = OUT_FRAMESTOBYTES(DTS3_FRAME_SIZE);
        else
          frameBytes = OUT_FRAMESTOBYTES(DTS1_FRAME_SIZE);
        const unsigned int capacity =
            wrapped ? frameBytes - IEC61937_DATA_OFFSET - DTSHD_PACKER_OVERHEAD
                    : frameBytes - IEC61937_DATA_OFFSET;
        const unsigned int deliveredSize =
            (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE) ? m_coreSize : m_fsize;

        if (deliveredSize > capacity && !(!wrapped && deliveredSize == frameBytes))
        {
          LOG_THROTTLE_PERIODIC_GENERAL(
              LOGINFO, 1000,
              "CAEStreamParser::SyncDTS - rejecting DTS reclassification: {} byte frame cannot "
              "fit its own {} byte burst payload (period {}), keeping established format",
              deliveredSize, capacity, candidatePeriod);
          return skip;
        }

        if (dataType != m_dtsCandidateType || sampleRate != m_dtsCandidateRate ||
            dtsBlocks != m_dtsCandidateBlocks)
        {
          m_dtsCandidateType = dataType;
          m_dtsCandidateRate = sampleRate;
          m_dtsCandidateBlocks = dtsBlocks;
          m_dtsChangeStreak = 0;
        }

        if (++m_dtsChangeStreak < DTS_CHANGE_CONFIRM_FRAMES)
        {
          LOG_THROTTLE_PERIODIC_GENERAL(
              LOGINFO, 1000,
              "CAEStreamParser::SyncDTS - holding established DTS format against "
              "reclassification until confirmed ({}/{} frames)",
              m_dtsChangeStreak, DTS_CHANGE_CONFIRM_FRAMES);
          return skip;
        }
      }
      m_dtsChangeStreak = 0;
      m_hasSync = true;
      m_info.m_dataIsLE = dataIsLE;
      m_info.m_type = dataType;
      m_info.m_sampleRate = sampleRate;
      m_dtsBlocks = dtsBlocks;
      m_info.m_channels = DTSChannels[amode] + (lfe ? 1 : 0);
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_info.m_repeat = 1;

      uint32_t hd_bits = 0;

      // If XLL aka DTS-HD Master Audio - Work out the bit depth
      if (ext_sub_sync == DTS_SYNC_EXT_XLL)
      {

        uint32_t bitPosition = 0;
        const uint8_t* hdBuffer = &data[m_coreSize + ext_header_size];

        auto ExtractBits = [&](uint32_t numBits) -> uint32_t {
            uint32_t result = 0;
            for (uint32_t i = 0; i < numBits; ++i)
            {
                uint32_t byteIndex = bitPosition / 8;
                uint32_t bitIndex = 7 - (bitPosition % 8);
                result = (result << 1) | ((hdBuffer[byteIndex] >> bitIndex) & 1);
                bitPosition++;
            }
            return result;
        };

        bitPosition = 32;  // Fast forward through bits to start after sub sync word

        // XLL Common Header
        uint32_t nVersion = ExtractBits(4) + 1;           // Version is 4 bits, add 1 to get actual version
        uint32_t nHeaderSize = ExtractBits(8) + 1;        // Header size is 8 bits, add 1 to get actual size (size is in bytes)
        uint32_t nBits4FrameFsize = ExtractBits(5) + 1;
        uint32_t nLLFrameSize = ExtractBits(nBits4FrameFsize) + 1;
        uint32_t nNumChSetsInFrame = ExtractBits(4) + 1;

        // Segments and samples calculation
        uint32_t tmp = ExtractBits(4);
        uint32_t nSegmentsInFrame = 1 << tmp;
        tmp = ExtractBits(4);
        uint32_t nSmplInSeg = 1 << tmp;

        // Calculate total samples per frame
        m_info.m_dtsSamplesPerFrame = (nSegmentsInFrame * nSmplInSeg);

        // Now find the offset to the first channel set sub header given the header size (in bytes).
        bitPosition = (nHeaderSize * 8);

        // Parse first Channel Set Sub-Header - to get the original audio data bit resolution
        uint32_t nSubHeaderSize = ExtractBits(10) + 1;    // Unpack the channel set sub header size
        uint32_t nChSetLLChannel = ExtractBits(4) + 1;    // Extract the number of channels
        bitPosition += nChSetLLChannel;                   // Skip Channels as bits!
        hd_bits = ExtractBits(5) + 1;                     // Extract the input sample bit resolution (bit depth)
      }

      m_info.m_bitDepth = (hd_bits > 0) ? hd_bits : bits;

      if (dataType != CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        m_info.m_hasDtsX = false;
        m_info.m_bedChannels = -1;
        m_info.m_bedIsLfeOnly = false;
        m_info.m_atmosObjects = -1;
        m_info.m_atmosChannels = 0;
      }

      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        unsigned int assetChannels = 0;
        unsigned int baseHeightChannels = 0;
        const bool assetParsed =
            DTS_ParseAssetChannels(data, size - skip, assetChannels, baseHeightChannels);
        const unsigned int coreChannels = m_info.m_channels;
        m_info.m_channels = assetParsed ? assetChannels : coreChannels + 2;
        m_info.m_hasDtsX = m_dtsX;
        m_info.m_bedChannels = -1;
        m_info.m_bedIsLfeOnly = false;
        m_info.m_atmosObjects = -1;

        static constexpr unsigned int DTSX_HEIGHT_CHANNELS = 4;
        const unsigned int dtsXHeights =
            (baseHeightChannels > 0) ? 0 : DTSX_HEIGHT_CHANNELS;
        m_info.m_atmosChannels = (m_dtsX && assetParsed) ? assetChannels + dtsXHeights : 0;
        if (m_dtsX && assetParsed)
        {
          m_info.m_bedChannels = static_cast<int>(assetChannels + dtsXHeights);
          m_info.m_bedIsLfeOnly = false;
        }

        const bool dtsXHint = m_dtsX;
        const unsigned int infoChannels = m_info.m_channels;
        const int bedChannels = m_info.m_bedChannels;
        const int atmosObjects = m_info.m_atmosObjects;
        const unsigned int atmosChannels = m_info.m_atmosChannels;
        logComponentM(LOGDEBUG, LOGAUDIO,
                      "dts asset descriptor: parsed={:d} dtsX={:d} nuTotalNumChs={} "
                      "coreChannels={} m_info.m_channels={} m_info.m_bedChannels={} "
                      "m_info.m_atmosObjects={} m_info.m_atmosChannels={}",
                      assetParsed, dtsXHint, assetChannels, coreChannels, infoChannels,
                      bedChannels, atmosObjects, atmosChannels);
        m_info.m_dtsPeriod = (192000 * (8 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
      {
        m_info.m_dtsPeriod = (192000 * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else
      {
        m_info.m_dtsPeriod =
            (m_info.m_sampleRate * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }

      if ((dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
           dataType == CAEStreamInfo::STREAM_TYPE_DTSHD) &&
          !IsPackableDtsBurst(m_info.m_dtsPeriod))
      {
        LOG_THROTTLE_PERIODIC_GENERAL(
            LOGWARNING, 1000,
            "CAEStreamParser::SyncDTS - DTS-HD period {} has no IEC 61937 burst subtype, "
            "Kodi-side IEC packing will drop every frame of this stream",
            m_info.m_dtsPeriod);
      }

      std::string type;
      switch (dataType)
      {
        case CAEStreamInfo::STREAM_TYPE_DTSHD:
          type = "dtsHD";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
          type = "dtsHD MA";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
          type = "dtsHD (core)";
          break;
        default:
          type = "dts";
          break;
      }

      if (extension)
      {
        switch (ext_type)
        {
          case 0:
            type += " XCH";
            break;
          case 2:
            type += " X96";
            break;
          case 6:
            type += " XXCH";
            break;
          default:
            type += " ext unknown";
            break;
        }
      }

      const std::string msg = fmt::format(
          "CAEStreamParser::SyncDTS - {} stream detected ({} channels, {}Hz, {}bit {}, "
          "period: {}, core syncword: 0x{:x}, ext syncword: 0x{:x}, ext sub syncword: 0x{:x}, target rate: 0x{:x}, framesize {}))",
          type, m_info.m_channels, m_info.m_sampleRate, m_info.m_bitDepth, m_info.m_dataIsLE ? "LE" : "BE",
          m_info.m_dtsPeriod, header, ext_sync, ext_sub_sync, target_rate, m_fsize);
      if (msg != m_lastLoggedStreamDetected)
      {
        CLog::Log(LOGINFO, "{}", msg);
        m_lastLoggedStreamDetected = msg;
      }
    }
    else
    {
      m_dtsChangeStreak = 0;
      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        unsigned int steadyAssetChannels = 0;
        unsigned int steadyHeightChannels = 0;
        if (DTS_ParseAssetChannels(data, size - skip, steadyAssetChannels, steadyHeightChannels) &&
            steadyAssetChannels != m_info.m_channels)
        {
          m_hasSync = false;
          return skip;
        }
      }
    }

    return skip;
  }

  // lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncDTS - DTS sync lost");
  m_hasSync = false;
  return skip;
}

inline unsigned int CAEStreamParser::GetTrueHDChannels(const uint16_t chanmap)
{
  int channels = 0;
  for (int i = 0; i < 13; ++i)
    channels += THDChanMap[i] * ((chanmap >> i) & 1);
  return channels;
}

unsigned int CAEStreamParser::SyncTrueHD(uint8_t* data, unsigned int size)
{
  unsigned int left = size;
  unsigned int skip = 0;

  // https://developer.dolby.com/globalassets/technology/dolby-truehd/dolbytruehdhighlevelbitstreamdescription.pdf

  // if TrueHD
  for (; left; ++skip, ++data, --left)
  {
    // if we dont have sync and there is less the 8 bytes, then break out
    if (!m_hasSync && left < 8)
      return size;

    // if its a major audio unit
    uint16_t length = ((data[0] & 0x0F) << 8 | data[1]) << 1;
    uint32_t syncword = ((((data[4] << 8 | data[5]) << 8) | data[6]) << 8) | data[7];
    if (syncword == 0xf8726fba)
    {
      // we need 32 bytes to sync on a master audio unit
      if (left < 32)
        return skip;

      // get the rate and ensure its valid
      int rate = (data[8] & 0xf0) >> 4;
      if (rate == 0xF)
        continue;

      unsigned int major_sync_size = 28;
      const bool extChannelMeaningPresent = (data[29] & 1) != 0;
      const int extension_count = extChannelMeaningPresent ? (data[30] >> 4) : 0;
      if (extChannelMeaningPresent)
        major_sync_size += 2 + extension_count * 2;

      if (left < 4 + major_sync_size)
        return skip;

      // verify the crc of the audio unit
      uint16_t crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
      crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
      if (((data[4 + major_sync_size - 1] << 8) | data[4 + major_sync_size - 2]) != crc)
        continue;

      // Detect Atmos and parse extra_channel_meaning fields (before any patching)
      // Bit layout inside extra_channel_meaning:
      //   extra_channel_meaning_length: 4 bits = data[30] bits[7:4]
      //   16ch_dialogue_norm:           5 bits = data[30] bits[3:0] + data[31] bit 7
      //   16ch_mix_level:               6 bits = data[31] bits[6:1]
      //   16ch_channel_count:           5 bits = data[31] bit 0   + data[32] bits[7:4]
      bool hasAtmos = (data[21] & 0x80) != 0;
      bool hasExtChannelMeaning = hasAtmos && extChannelMeaningPresent && extension_count >= 1;
      int origDialNorm = 0;
      int atmosChannels = 0;
      int atmosObjects = -1;
      int atmosBedChannels = -1;
      bool atmosBedIsLfeOnly = false;
      int atmosDerivedObjects = -1;

      if (hasExtChannelMeaning)
      {
        // 5-bit raw value 0-31 maps to -31 to 0 dB (dB = raw - 31)
        int dialNormRaw = ((data[30] & 0x0F) << 1) | (data[31] >> 7);
        origDialNorm = dialNormRaw - 31;

        int channelCountRaw = ((data[31] & 0x01) << 4) | (data[32] >> 4);
        atmosChannels = channelCountRaw + 1;

        const unsigned int extChannelMeaningEndBit = 240 + (extension_count + 1) * 16;
        AtmosBitReader br(data, 260, extChannelMeaningEndBit, left);
        const AtmosProgram prog =
            Atmos_ParseProgramAssignment(br, atmosChannels, AtmosCarriage::TRUEHD);
        atmosObjects = prog.declaredObjects;
        atmosBedChannels = prog.objectOnlyProgram ? 0 : prog.bedChannels;
        atmosBedIsLfeOnly = prog.objectOnlyProgram && prog.bedChannels == 1;
        atmosDerivedObjects = Atmos_DerivedObjects(prog, atmosChannels);
      }

      // Defeat 16ch dialog normalization to 0 dB on every major sync frame.
      // This disables receiver-side dialog normalization for the Atmos presentation.
      bool dialNormDefeated = false;
      if (m_defeatTrueHDDialNorm && hasExtChannelMeaning && origDialNorm != 0)
      {
        data[30] |= 0x0F; // set dialnorm bits [4:1] = 1111
        data[31] |= 0x80; // set dialnorm bit  [0]   = 1
        dialNormDefeated = true;

        // Recompute CRC-16 (polynomial 0x002D) after modification
        uint16_t new_crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
        new_crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
        data[4 + major_sync_size - 2] = new_crc & 0xFF;
        data[4 + major_sync_size - 1] = (new_crc >> 8) & 0xFF;
      }

      m_substreams = (data[20] & 0xF0) >> 4;
      m_fsize = length;

      const bool firstSync = !m_hasSync;

      if (!m_hasSync)
      {
        // Looks like cannot understand the original bit depth - can only assume it is (up-to) 24 bit!
        // DTS-MA has the original bit depth from the PCM for example.
        // Seen some attempts at calculation e.g. from BDInfo but not sure that can be correct as
        // with lossless compressed audio the bit rate will vary, but the original bit depth should be constant.
        // Would need to extract the samples and see if they were all padded to tell, but then seen
        // comments that some titles will use 16 padded to 24 in some scenes and then use full 24 in others!
        // so overall just go with 24!
        m_info.m_bitDepth = 24;

        // get the sample rate and substreams, we have a valid master audio unit
        m_info.m_sampleRate = (rate & 0x8 ? 44100 : 48000) << (rate & 0x7);

        // get the number of encoded channels
        uint16_t channel_map = ((data[10] & 0x1F) << 8) | data[11];
        if (!channel_map)
          channel_map = (data[9] << 1) | (data[10] >> 7);
        m_info.m_channels = CAEStreamParser::GetTrueHDChannels(channel_map);

        m_hasSync = true;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
        m_syncFunc = &CAEStreamParser::SyncTrueHD;
        m_info.m_repeat = 1;
      }

      m_info.m_hasAtmos = hasAtmos;
      if (hasExtChannelMeaning)
      {
        m_info.m_dialNorm = origDialNorm;
        m_info.m_dialNormApplied = dialNormDefeated ? 0 : origDialNorm;
        m_info.m_hasDialNorm = true;
        m_info.m_atmosChannels = atmosChannels;
        m_info.m_atmosObjects = atmosObjects;
        m_info.m_bedChannels = atmosBedChannels;
        m_info.m_bedIsLfeOnly = atmosBedIsLfeOnly;
      }
      else if (!hasAtmos || firstSync)
      {
        m_info.m_dialNorm = 0;
        m_info.m_dialNormApplied = 0;
        m_info.m_hasDialNorm = true;
        m_info.m_atmosChannels = 0;
        m_info.m_atmosObjects = -1;
        m_info.m_bedChannels = -1;
        m_info.m_bedIsLfeOnly = false;
      }

      const std::array<int, 10> logKey{static_cast<int>(m_info.m_channels),
                                       static_cast<int>(m_info.m_sampleRate),
                                       static_cast<int>(m_info.m_bitDepth),
                                       hasAtmos ? 1 : 0,
                                       dialNormDefeated ? 1 : 0,
                                       m_info.m_dialNorm,
                                       m_info.m_dialNormApplied,
                                       static_cast<int>(m_info.m_atmosChannels),
                                       m_info.m_atmosObjects,
                                       atmosDerivedObjects};
      if (!m_hasTrueHDLogKey || logKey != m_lastTrueHDLogKey)
      {
        m_hasTrueHDLogKey = true;
        m_lastTrueHDLogKey = logKey;

        std::string atmosStr;
        if (hasAtmos)
        {
          atmosStr = ", dialNorm: " + std::to_string(m_info.m_dialNormApplied) + " dB";
          if (dialNormDefeated)
            atmosStr += " (defeated from " + std::to_string(origDialNorm) + " dB)";
          if (m_info.m_atmosChannels)
            atmosStr += ", atmosChannels: " + std::to_string(m_info.m_atmosChannels);
          if (m_info.m_atmosObjects >= 0)
            atmosStr += ", atmosObjects: " + std::to_string(m_info.m_atmosObjects);
          if (atmosDerivedObjects >= 0 && atmosDerivedObjects != m_info.m_atmosObjects)
            atmosStr += ", derivedObjects: " + std::to_string(atmosDerivedObjects);
        }
        const std::string msg = fmt::format(
            "CAEStreamParser::SyncTrueHD - TrueHD stream detected ({} channels, {}Hz, "
            "{}-bit{}{})",
            m_info.m_channels, m_info.m_sampleRate, m_info.m_bitDepth,
            hasAtmos ? ", Atmos" : "", atmosStr);
        if (msg != m_lastLoggedStreamDetected)
        {
          CLog::Log(LOGINFO, "{}", msg);
          m_lastLoggedStreamDetected = msg;
        }
      }

      return skip;
    }
    else
    {
      // we cant sink to a subframe until we have the information from a master audio unit
      if (!m_hasSync)
        continue;

      // if there is not enough data left to verify the packet, just return the skip amount
      if (left < static_cast<unsigned int>(m_substreams) * 4)
        return skip;

      // verify the parity
      int p = 0;
      uint8_t check = 0;
      for (int i = -1; i < m_substreams; ++i)
      {
        check ^= data[p++];
        check ^= data[p++];
        if (i == -1 || data[p - 2] & 0x80)
        {
          check ^= data[p++];
          check ^= data[p++];
        }
      }

      // if the parity nibble does not match
      if ((((check >> 4) ^ check) & 0xF) != 0xF)
      {
        // lost sync
        m_hasSync = false;
        CLog::Log(LOGINFO, "CAEStreamParser::SyncTrueHD - Sync Lost");
        continue;
      }
      else
      {
        m_fsize = length;
        return skip;
      }
    }
  }

  // lost sync
  m_hasSync = false;
  return skip;
}