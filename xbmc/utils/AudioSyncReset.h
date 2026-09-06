/*
 *  Copyright (C) 2026-present Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>

class CAudioSyncReset
{
public:
  static CAudioSyncReset& GetInstance();

  void SetAlgoForReset(int num_resets) { m_algoForReset = num_resets; }
  int GetAlgoForReset() const { return m_algoForReset; }

  void SetLastResetTime(double reset_time) { m_lastResetTime = reset_time; }
  double GetLastResetTime() const { return m_lastResetTime; }

  void SetResetSync(bool reset_sync) { m_resetSync = reset_sync; }
  bool GetResetSync() const { return m_resetSync; }
  bool TakeResetSync() { return m_resetSync.exchange(false); }

  void SetResetSeek(bool reset_seek) { m_resetSeek = reset_seek; }
  bool GetResetSeek() const { return m_resetSeek; }

  void SetAlgoForResetSub(int num_resets) { m_algoForResetSub = num_resets; }
  int GetAlgoForResetSub() const { return m_algoForResetSub; }

  void SetLastResetTimeSub(double reset_time) { m_lastResetTimeSub = reset_time; }
  double GetLastResetTimeSub() const { return m_lastResetTimeSub; }

  void SetResetSyncSub(bool reset_sync) { m_resetSyncSub = reset_sync; }
  bool GetResetSyncSub() const { return m_resetSyncSub; }

  void SetResetSeekSub(bool reset_seek) { m_resetSeekSub = reset_seek; }
  bool GetResetSeekSub() const { return m_resetSeekSub; }

private:
  CAudioSyncReset() = default;

  std::atomic<int> m_algoForReset{0};
  std::atomic<double> m_lastResetTime{0.0};
  std::atomic<bool> m_resetSync{false};
  std::atomic<bool> m_resetSeek{false};
  std::atomic<int> m_algoForResetSub{0};
  std::atomic<double> m_lastResetTimeSub{0.0};
  std::atomic<bool> m_resetSyncSub{false};
  std::atomic<bool> m_resetSeekSub{false};
};
