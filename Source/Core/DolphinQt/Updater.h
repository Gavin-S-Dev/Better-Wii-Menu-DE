// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <QThread>
#include <string>
class QWidget;
// Checks fork releases only; never invokes the stock in-place updater.
class Updater final : public QThread
{
  Q_OBJECT
public:
  explicit Updater(QWidget* parent, std::string update_track, std::string hash_override);
  void run() override;
  void CheckForUpdate();
signals:
  void Result(const QString& tag, const QString& error);

private:
  bool m_enabled;
  bool m_manual = false;
};
