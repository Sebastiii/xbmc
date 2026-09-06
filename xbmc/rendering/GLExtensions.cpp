/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GLExtensions.h"

#include "ServiceBroker.h"
#include "rendering/RenderSystem.h"

#include <algorithm>
#include <unordered_set>

bool CGLExtensions::IsExtensionSupported(CGLExtensions::Extension extension)
{
  static const std::unordered_set<CGLExtensions::Extension> supportedExtensions = []()
  {
    auto isExtSupported = [](const auto& tmp)
    { return CServiceBroker::GetRenderSystem()->IsExtSupported(std::string(tmp.second).c_str()); };
    
    std::unordered_set<CGLExtensions::Extension> result;
    for (const auto& entry : CGLExtensions::stringMap)
    {
      if (isExtSupported(entry))
      {
        result.insert(entry.first);
      }
    }
    return result;
  }();

  return supportedExtensions.find(extension) != supportedExtensions.end();
}
