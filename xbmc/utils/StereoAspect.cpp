/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "StereoAspect.h"

#include <cmath>

namespace
{
constexpr double SBS_HALF_EYE_MAX = 1.27;
constexpr double SBS_FULL_EYE_MIN = 1.40;
constexpr double TAB_FULL_EYE_MAX = 2.5;
constexpr double TAB_HALF_EYE_MIN = 2.8;
constexpr int HD_MAX_WIDTH = 1920;
constexpr int HD_MAX_HEIGHT = 1080;
constexpr double DECLARED_TOLERANCE = 1e-3;

bool IsDeclared(double displayAspect, double frameAspect)
{
  return displayAspect > 0.0 &&
         std::fabs(displayAspect - frameAspect) > frameAspect * DECLARED_TOLERANCE;
}
}

bool StereoAspect::IsSideBySide(std::string_view stereoMode)
{
  return stereoMode == "left_right" || stereoMode == "right_left";
}

bool StereoAspect::IsTopBottom(std::string_view stereoMode)
{
  return stereoMode == "top_bottom" || stereoMode == "bottom_top";
}

double StereoAspect::MatroskaSampleAspectFactor(std::string_view stereoMode)
{
  if (IsSideBySide(stereoMode))
    return 2.0;
  if (IsTopBottom(stereoMode))
    return 0.5;
  return 1.0;
}

double StereoAspect::PerEyeAspect(std::string_view stereoMode,
                                  int width,
                                  int height,
                                  double displayAspect)
{
  if (width <= 0 || height <= 0)
    return displayAspect;

  const double frameAspect = static_cast<double>(width) / height;
  const bool sideBySide = IsSideBySide(stereoMode);
  const bool topBottom = IsTopBottom(stereoMode);

  if (!sideBySide && !topBottom)
    return displayAspect > 0.0 ? displayAspect : frameAspect;

  if (IsDeclared(displayAspect, frameAspect))
    return displayAspect;

  if (sideBySide)
  {
    const double eyeAspect = frameAspect / 2.0;
    const bool half = eyeAspect < SBS_HALF_EYE_MAX ||
                      (eyeAspect < SBS_FULL_EYE_MIN &&
                       (width <= HD_MAX_WIDTH || height > HD_MAX_HEIGHT));
    return half ? frameAspect : eyeAspect;
  }

  const double eyeAspect = frameAspect * 2.0;
  const bool half = eyeAspect > TAB_HALF_EYE_MIN ||
                    (eyeAspect > TAB_FULL_EYE_MAX && height <= HD_MAX_HEIGHT);
  return half ? frameAspect : eyeAspect;
}
