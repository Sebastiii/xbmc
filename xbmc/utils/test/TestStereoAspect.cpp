/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/StereoAspect.h"

#include <gtest/gtest.h>

namespace
{
struct StereoAspectCase
{
  const char* name;
  const char* mode;
  int width;
  int height;
  double displayAspect;
  double expected;
};

const StereoAspectCase cases[] = {
    {"hsbs 16:9 1080p default dims", "left_right", 1920, 1080, 1920.0 / 1080, 16.0 / 9},
    {"hsbs 16:9 720p default dims", "left_right", 1280, 720, 1280.0 / 720, 16.0 / 9},
    {"hsbs 16:9 2160p default dims", "left_right", 3840, 2160, 3840.0 / 2160, 16.0 / 9},
    {"hsbs 4:3 default dims", "left_right", 1440, 1080, 1440.0 / 1080, 4.0 / 3},
    {"hsbs 2.40 cropped default dims", "left_right", 1920, 800, 1920.0 / 800, 2.4},
    {"hsbs 2.76 cropped default dims", "left_right", 1920, 696, 1920.0 / 696, 1920.0 / 696},
    {"hsbs 2.76 cropped 2160p default dims", "left_right", 3840, 1392, 3840.0 / 1392,
     3840.0 / 1392},
    {"hsbs 2.55 cropped 2160p default dims", "left_right", 3840, 1506, 3840.0 / 1506,
     3840.0 / 1506},
    {"hsbs declared 2.40 on 1080p frame", "left_right", 1920, 1080, 2.4, 2.4},
    {"fsbs 16:9 default dims", "left_right", 3840, 1080, 3840.0 / 1080, 16.0 / 9},
    {"fsbs 16:9 720p default dims", "left_right", 2560, 720, 2560.0 / 720, 16.0 / 9},
    {"fsbs 16:9 2160p default dims", "left_right", 7680, 2160, 7680.0 / 2160, 16.0 / 9},
    {"fsbs 2.40 cropped default dims", "left_right", 3840, 800, 3840.0 / 800, 2.4},
    {"fsbs 4:3 default dims", "left_right", 2880, 1080, 2880.0 / 1080, 4.0 / 3},
    {"fsbs declared per-eye dims", "left_right", 3840, 1080, 16.0 / 9, 16.0 / 9},
    {"fsbs right_left", "right_left", 3840, 1080, 3840.0 / 1080, 16.0 / 9},
    {"fsbs no display aspect", "left_right", 3840, 1080, 0.0, 16.0 / 9},
    {"htab 16:9 1080p default dims", "top_bottom", 1920, 1080, 1920.0 / 1080, 16.0 / 9},
    {"htab 16:9 720p default dims", "top_bottom", 1280, 720, 1280.0 / 720, 16.0 / 9},
    {"htab 4:3 default dims", "top_bottom", 1440, 1080, 1440.0 / 1080, 4.0 / 3},
    {"htab 2.40 cropped default dims", "top_bottom", 1920, 800, 1920.0 / 800, 2.4},
    {"ftab 16:9 default dims", "top_bottom", 1920, 2160, 1920.0 / 2160, 16.0 / 9},
    {"ftab 16:9 720p default dims", "top_bottom", 1280, 1440, 1280.0 / 1440, 16.0 / 9},
    {"ftab 2.40 default dims", "top_bottom", 1920, 1600, 1920.0 / 1600, 2.4},
    {"ftab 2.76 default dims", "top_bottom", 1920, 1392, 1920.0 / 1392, 1920.0 / 696},
    {"ftab declared per-eye dims", "top_bottom", 1920, 2160, 16.0 / 9, 16.0 / 9},
    {"ftab bottom_top", "bottom_top", 1920, 2160, 1920.0 / 2160, 16.0 / 9},
    {"mono keeps display aspect", "mono", 1920, 1080, 2.4, 2.4},
    {"mono no display aspect", "", 1920, 1080, 0.0, 16.0 / 9},
    {"mvc keeps display aspect", "block_lr", 1920, 1080, 16.0 / 9, 16.0 / 9},
    {"zero size keeps display aspect", "left_right", 0, 0, 2.4, 2.4},
};
}

TEST(TestStereoAspect, PerEyeAspect)
{
  for (const auto& c : cases)
  {
    SCOPED_TRACE(c.name);
    EXPECT_NEAR(c.expected, StereoAspect::PerEyeAspect(c.mode, c.width, c.height, c.displayAspect),
                1e-3);
  }
}

TEST(TestStereoAspect, MatroskaSampleAspectFactor)
{
  EXPECT_DOUBLE_EQ(2.0, StereoAspect::MatroskaSampleAspectFactor("left_right"));
  EXPECT_DOUBLE_EQ(2.0, StereoAspect::MatroskaSampleAspectFactor("right_left"));
  EXPECT_DOUBLE_EQ(0.5, StereoAspect::MatroskaSampleAspectFactor("top_bottom"));
  EXPECT_DOUBLE_EQ(0.5, StereoAspect::MatroskaSampleAspectFactor("bottom_top"));
  EXPECT_DOUBLE_EQ(1.0, StereoAspect::MatroskaSampleAspectFactor("block_lr"));
  EXPECT_DOUBLE_EQ(1.0, StereoAspect::MatroskaSampleAspectFactor(""));
}

namespace
{
double MatroskaPerEye(const char* mode, int width, int height, double sampleAspect)
{
  const double frameAspect = static_cast<double>(width) / height;
  const double displayAspect =
      sampleAspect / StereoAspect::MatroskaSampleAspectFactor(mode) * frameAspect;
  return StereoAspect::PerEyeAspect(mode, width, height, displayAspect);
}
}

TEST(TestStereoAspect, MatroskaRoundTrip)
{
  EXPECT_NEAR(16.0 / 9, MatroskaPerEye("left_right", 3840, 1080, 2.0), 1e-3);
  EXPECT_NEAR(16.0 / 9, MatroskaPerEye("left_right", 1920, 1080, 2.0), 1e-3);
  EXPECT_NEAR(16.0 / 9, MatroskaPerEye("left_right", 3840, 1080, 1.0), 1e-3);
  EXPECT_NEAR(16.0 / 9, MatroskaPerEye("top_bottom", 1920, 2160, 0.5), 1e-3);
  EXPECT_NEAR(16.0 / 9, MatroskaPerEye("top_bottom", 1920, 1080, 0.5), 1e-3);
  EXPECT_NEAR(3840.0 / 1392, MatroskaPerEye("left_right", 3840, 1392, 2.0), 1e-3);
}
