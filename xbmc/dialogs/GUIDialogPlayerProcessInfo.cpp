/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogPlayerProcessInfo.h"

#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"

namespace
{
constexpr unsigned int PROCESS_INFO_UPDATE_INTERVAL_MS = 167;
}

CGUIDialogPlayerProcessInfo::CGUIDialogPlayerProcessInfo(void)
    : CGUIDialog(WINDOW_DIALOG_PLAYER_PROCESS_INFO, "DialogPlayerProcessInfo.xml")
{
  m_loadType = KEEP_IN_MEMORY;
}

CGUIDialogPlayerProcessInfo::~CGUIDialogPlayerProcessInfo(void) = default;

bool CGUIDialogPlayerProcessInfo::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_PLAYER_PROCESS_INFO)
  {
    Close();
    return true;
  }
  return CGUIDialog::OnAction(action);
}

void CGUIDialogPlayerProcessInfo::Process(unsigned int currentTime, CDirtyRegionList &dirtyregions)
{
  if (currentTime - m_lastProcessTime < PROCESS_INFO_UPDATE_INTERVAL_MS &&
      !IsAnimating(ANIM_TYPE_WINDOW_OPEN) && !IsAnimating(ANIM_TYPE_WINDOW_CLOSE))
    return;

  m_lastProcessTime = currentTime;
  CGUIDialog::Process(currentTime, dirtyregions);
}
