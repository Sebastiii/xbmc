/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ProcessInfoAmlogic.h"

using namespace VIDEOPLAYER;

CProcessInfo* CProcessInfoAmlogic::Create()
{
  return new CProcessInfoAmlogic();
}

void CProcessInfoAmlogic::Register()
{
  CProcessInfo::RegisterProcessControl("aml", CProcessInfoAmlogic::Create);
}

EINTERLACEMETHOD CProcessInfoAmlogic::GetFallbackDeintMethod()
{
  int width = 0;
  int height = 0;
  GetVideoDimensions(width, height);

  if (height > 576 || width > 720)
    return EINTERLACEMETHOD::VS_INTERLACEMETHOD_DEINTERLACE_HALF;

  return CProcessInfo::GetFallbackDeintMethod();
}
