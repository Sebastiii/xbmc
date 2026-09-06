/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ServiceBroker.h"
#include "utils/log.h"

#include <chrono>

namespace BDSTAGE
{
using Clock = std::chrono::steady_clock;

inline bool g_on{false};
inline int g_n{0};
inline int g_playlist{-1};
inline bool g_inMenu{false};
inline Clock::time_point g_play{};
inline Clock::time_point g_prevShown{};
inline Clock::time_point g_plChange{};
inline Clock::time_point g_open{};
inline Clock::time_point g_emit{};

inline bool Unset(const Clock::time_point& t)
{
  return t.time_since_epoch().count() == 0;
}

inline int Ms(const Clock::time_point& a, const Clock::time_point& b)
{
  if (Unset(a) || Unset(b) || b < a)
    return -1;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
}

inline void Play()
{
  g_on = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
         CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO);
  if (!g_on)
    return;
  g_n = 0;
  g_playlist = -1;
  g_inMenu = false;
  g_play = Clock::now();
  g_prevShown = g_play;
  g_plChange = {};
  g_open = {};
  g_emit = {};
}

inline void Playlist(int id, bool inMenu)
{
  if (!g_on)
    return;
  g_playlist = id;
  g_inMenu = inMenu;
  if (Unset(g_plChange))
    g_plChange = Clock::now();
}

inline void DecoderOpen()
{
  if (!g_on)
    return;
  if (Unset(g_open))
    g_open = Clock::now();
}

inline void PictureEmitted()
{
  if (!g_on)
    return;
  if (Unset(g_emit))
    g_emit = Clock::now();
}

inline void PictureShown()
{
  if (!g_on)
    return;
  if (g_n != 0 && Unset(g_plChange))
    return;

  const Clock::time_point now = Clock::now();
  ++g_n;
  logComponentM(LOGDEBUG, LOGVIDEO,
                "bdstage: n={} playlist={} inMenu={} sincePlay={} sincePrev={} vm={} open={} "
                "decode={} hold={}",
                g_n, g_playlist, g_inMenu, Ms(g_play, now), Ms(g_prevShown, now),
                Ms(g_prevShown, g_plChange), Ms(g_plChange, g_open), Ms(g_open, g_emit),
                Ms(g_emit, now));
  g_prevShown = now;
  g_plChange = {};
  g_open = {};
  g_emit = {};
}
} // namespace BDSTAGE
