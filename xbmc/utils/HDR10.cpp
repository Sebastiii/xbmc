/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Colour primary names follow the informative remarks of Rec. ITU-T H.273 (V4)
 *  (07/2024) Table 2. The mastering display colour volume element order is
 *  specified by Rec. ITU-T H.265 clause D.3.27, where index 0 is green, index 1
 *  is blue and index 2 is red. Match tolerances are fixed at half of the finest
 *  precision H.273 Table 2 quotes: 0.0005 for the primaries, which is 25 units
 *  of 0.00002, and 0.00005 for the white point, rounded up to 3 units. Entries
 *  the table quotes more coarsely are therefore matched more strictly.
 */

#include "HDR10.h"

#include <array>
#include <string>

namespace
{
constexpr int PRIMARY_TOLERANCE = 25;
constexpr int WHITE_TOLERANCE = 3;

constexpr size_t GREEN = 0;
constexpr size_t BLUE = 1;
constexpr size_t RED = 2;

bool Matches(const DisplayPrimary& point, const uint16_t (&reference)[2], int tolerance)
{
  const int dx = static_cast<int>(point.x) - static_cast<int>(reference[0]);
  const int dy = static_cast<int>(point.y) - static_cast<int>(reference[1]);

  return dx >= -tolerance && dx <= tolerance && dy >= -tolerance && dy <= tolerance;
}

std::string Coordinates(const DisplayPrimary& point)
{
  return std::to_string(point.x) + "," + std::to_string(point.y);
}
} // namespace

std::string CodeToColourPrimaries(uint8_t code)
{
  switch (code)
  {
    case 1:
      return "BT.709";
    case 4:
      return "BT.470 System M";
    case 5:
      return "BT.601 PAL";
    case 6:
      return "BT.601 NTSC";
    case 7:
      return "SMPTE 240M";
    case 8:
      return "Generic film";
    case 9:
      return "BT.2020";
    case 10:
      return "XYZ";
    case 11:
      return "DCI P3";
    case 12:
      return "Display P3";
    case 22:
      return "EBU Tech 3213";
    default:
      return "";
  }
}

std::string MasteringDisplayColourVolumeText(const MasteringDisplayColourVolume& mdcv)
{
  static constexpr std::array<std::array<size_t, 3>, 6> orderings = {
      {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};

  for (const auto& set : COLOUR_PRIMARY_SETS)
  {
    if (!Matches(mdcv.whitePoint, set.white, WHITE_TOLERANCE))
      continue;

    for (const auto& order : orderings)
    {
      if (Matches(mdcv.displayPrimaries[order[0]], set.green, PRIMARY_TOLERANCE) &&
          Matches(mdcv.displayPrimaries[order[1]], set.blue, PRIMARY_TOLERANCE) &&
          Matches(mdcv.displayPrimaries[order[2]], set.red, PRIMARY_TOLERANCE))
        return CodeToColourPrimaries(set.code);
    }
  }

  return "R:" + Coordinates(mdcv.displayPrimaries[RED]) + " " +
         "G:" + Coordinates(mdcv.displayPrimaries[GREEN]) + " " +
         "B:" + Coordinates(mdcv.displayPrimaries[BLUE]) + " " +
         "W:" + Coordinates(mdcv.whitePoint);
}
