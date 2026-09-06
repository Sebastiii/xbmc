/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string_view>

namespace StereoAspect
{
bool IsSideBySide(std::string_view stereoMode);
bool IsTopBottom(std::string_view stereoMode);
double MatroskaSampleAspectFactor(std::string_view stereoMode);
double PerEyeAspect(std::string_view stereoMode, int width, int height, double displayAspect);
}
