/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Colour primary code points and chromaticity coordinates are taken from
 *  Rec. ITU-T H.273 (V4) (07/2024) Table 2 "Interpretation of colour primaries
 *  (ColourPrimaries) value". Coordinates are stored in the units used by the
 *  mastering display colour volume SEI message, i.e. increments of 0.00002,
 *  so each table entry is the H.273 coordinate multiplied by 50000.
 */

#pragma once

#include "HevcSei.h"

#include <cstdint>
#include <string>

struct ColourPrimarySet
{
  uint8_t code;
  uint16_t green[2];
  uint16_t blue[2];
  uint16_t red[2];
  uint16_t white[2];
};

inline constexpr ColourPrimarySet COLOUR_PRIMARY_SETS[] = {
    {1, {15000, 30000}, {7500, 3000}, {32000, 16500}, {15635, 16450}},
    {4, {10500, 35500}, {7000, 4000}, {33500, 16500}, {15500, 15800}},
    {5, {14500, 30000}, {7500, 3000}, {32000, 16500}, {15635, 16450}},
    {6, {15500, 29750}, {7750, 3500}, {31500, 17000}, {15635, 16450}},
    {9, {8500, 39850}, {6550, 2300}, {35400, 14600}, {15635, 16450}},
    {11, {13250, 34500}, {7500, 3000}, {34000, 16000}, {15700, 17550}},
    {12, {13250, 34500}, {7500, 3000}, {34000, 16000}, {15635, 16450}},
    {22, {14750, 30250}, {7750, 3850}, {31500, 17000}, {15635, 16450}},
};

std::string CodeToColourPrimaries(uint8_t code);
std::string MasteringDisplayColourVolumeText(const MasteringDisplayColourVolume& mdcv);
