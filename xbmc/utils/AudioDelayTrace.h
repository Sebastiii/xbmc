/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/log.h"

#include <chrono>

namespace AudioDelayTrace
{
inline double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}

#define AUDIODELAY_LOG(phase, fmt, ...) \
  do \
  { \
    if (CServiceBroker::GetLogging().CanLogComponent(LOGAUDIO)) \
    { \
      static auto s_audiodelay_last = std::chrono::steady_clock::time_point{}; \
      const auto s_audiodelay_now = std::chrono::steady_clock::now(); \
      if (s_audiodelay_now - s_audiodelay_last >= std::chrono::milliseconds(100)) \
      { \
        logComponentM(LOGDEBUG, LOGAUDIO, \
                      "AUDIODELAY t={:.3f} phase=" phase " " fmt, \
                      AudioDelayTrace::NowMs(), ##__VA_ARGS__); \
        s_audiodelay_last = s_audiodelay_now; \
      } \
    } \
  } while (0)

#define AUDIODELAY_ENABLED() \
  (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) && \
   CServiceBroker::GetLogging().CanLogComponent(LOGAUDIO))
