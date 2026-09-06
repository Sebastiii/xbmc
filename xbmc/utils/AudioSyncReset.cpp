/*
 *  Copyright (C) 2026-present Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioSyncReset.h"

CAudioSyncReset& CAudioSyncReset::GetInstance()
{
  static CAudioSyncReset s_instance;
  return s_instance;
}
