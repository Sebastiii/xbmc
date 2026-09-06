/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "StreamUtils.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
}

std::string StreamUtils::GetCanonicalCodecName(const std::string& codec)
{
  if (codec == "truehd_atmos")
    return "truehd";
  if (codec == "eac3_ddp_atmos")
    return "eac3";
  if (codec == "dtshd_ma_x" || codec == "dtshd_ma_x_imax")
    return "dtshd_ma";
  if (codec == "dts_es" || codec == "dts_96_24" || codec == "dts_express")
    return "dca";
  if (codec == "aac_lc" || codec == "he_aac" || codec == "he_aac_v2" || codec == "aac_ssr" ||
      codec == "aac_ltp")
    return "aac";

  return codec;
}

std::string StreamUtils::GetCodecDetail(const std::string& codec)
{
  if (codec == "truehd_atmos" || codec == "eac3_ddp_atmos")
    return "Dolby Atmos";
  if (codec == "dtshd_ma_x")
    return "DTS:X";
  if (codec == "dtshd_ma_x_imax")
    return "DTS:X IMAX";
  if (codec == "dts_es")
    return "DTS-ES";
  if (codec == "dts_96_24")
    return "DTS 96/24";
  if (codec == "dts_express")
    return "DTS Express";
  if (codec == "aac_lc")
    return "AAC-LC";
  if (codec == "he_aac")
    return "HE-AAC";
  if (codec == "he_aac_v2")
    return "HE-AAC v2";
  if (codec == "aac_ssr")
    return "AAC-SSR";
  if (codec == "aac_ltp")
    return "AAC-LTP";

  return {};
}

std::string StreamUtils::GetExtendedCodecName(const std::string& codec,
                                             const std::string& profile)
{
  if (profile == "Dolby Atmos")
  {
    if (codec == "truehd")
      return "truehd_atmos";
    if (codec == "eac3")
      return "eac3_ddp_atmos";
  }
  else if (profile == "DTS:X")
  {
    if (codec == "dtshd_ma")
      return "dtshd_ma_x";
  }
  else if (profile == "DTS:X IMAX")
  {
    if (codec == "dtshd_ma")
      return "dtshd_ma_x_imax";
  }

  return codec;
}

int StreamUtils::GetCodecPriority(const std::string &codec)
{
  /*
   * Technically flac, truehd, and dtshd_ma are equivalently good as they're all lossless. However,
   * ffmpeg can't decode dtshd_ma losslessy yet.
   */
  if (codec == "truehd_atmos") // Dolby TrueHD with Atmos
    return 11;
  if (codec == "dtshd_ma_x_imax") // DTS:X IMAX Enhanced
    return 10;
  if (codec == "dtshd_ma_x") // DTS:X
    return 9;
  if (codec == "dts_x" || codec == "dtsx" || codec == "dts-x" || codec == "dts:x")
    return 9;
  if (codec == "flac") // Lossless FLAC
    return 8;
  if (codec == "pcm")
    return 8;
  if (codec == "truehd") // Dolby TrueHD
    return 7;
  if (codec == "dtshd_ma") // DTS-HD Master Audio (previously known as DTS++)
    return 6;
  if (codec == "dtshd_hra") // DTS-HD High Resolution Audio
    return 5;
  if (codec == "eac3_ddp_atmos") // Dolby Digital Plus with Atmos
    return 4;
  if (codec == "eac3") // Dolby Digital Plus
    return 3;
  if (codec == "dts_es")
    return 2;
  if (codec == "dts_96_24")
    return 2;
  if (codec == "dts_express")
    return 2;
  if (codec == "dca") // DTS
    return 2;
  if (codec == "dts")
    return 2;
  if (codec == "ac3") // Dolby Digital
    return 1;
  return 0;
}

std::string StreamUtils::GetCodecName(int codecId, int profile)
{
  std::string codecName;

  if (codecId == AV_CODEC_ID_DTS)
  {
    if (profile == AV_PROFILE_DTS_HD_MA)
      codecName = "dtshd_ma";
    else if (profile == AV_PROFILE_DTS_HD_MA_X)
      codecName = "dtshd_ma_x";
    else if (profile == AV_PROFILE_DTS_HD_MA_X_IMAX)
      codecName = "dtshd_ma_x_imax";
    else if (profile == AV_PROFILE_DTS_HD_HRA)
      codecName = "dtshd_hra";
    else if (profile == AV_PROFILE_DTS_ES)
      codecName = "dts_es";
    else if (profile == AV_PROFILE_DTS_96_24)
      codecName = "dts_96_24";
    else if (profile == AV_PROFILE_DTS_EXPRESS)
      codecName = "dts_express";
    else
      codecName = "dca";

    return codecName;
  }

  if (codecId == AV_CODEC_ID_AAC)
  {
    switch (profile)
    {
      case AV_PROFILE_AAC_LOW:
      case AV_PROFILE_MPEG2_AAC_LOW:
        codecName = "aac_lc";
        break;
      case AV_PROFILE_AAC_HE:
      case AV_PROFILE_MPEG2_AAC_HE:
        codecName = "he_aac";
        break;
      case AV_PROFILE_AAC_HE_V2:
        codecName = "he_aac_v2";
        break;
      case AV_PROFILE_AAC_SSR:
        codecName = "aac_ssr";
        break;
      case AV_PROFILE_AAC_LTP:
        codecName = "aac_ltp";
        break;
      default:
        codecName = "aac";
    }
    return codecName;
  }

  if (codecId == AV_CODEC_ID_EAC3 && profile == AV_PROFILE_EAC3_DDP_ATMOS)
    return "eac3_ddp_atmos";

  if (codecId == AV_CODEC_ID_TRUEHD && profile == AV_PROFILE_TRUEHD_ATMOS)
    return "truehd_atmos";

  const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codecId));
  if (codec)
    codecName = avcodec_get_name(codec->id);

  return codecName;
}
