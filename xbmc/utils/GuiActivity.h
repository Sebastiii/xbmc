/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <chrono>

namespace KODI
{
namespace UTILS
{
namespace GUIACTIVITY
{

inline std::atomic<int> g_idleSeconds{0};

inline std::chrono::milliseconds HeartbeatInterval()
{
  return std::chrono::milliseconds(g_idleSeconds.load(std::memory_order_relaxed) >= 10 ? 10000
                                                                                        : 1000);
}

}
}
}
