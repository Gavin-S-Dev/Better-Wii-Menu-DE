// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinQt/Updater.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <QWidget>
#include <chrono>
#include "Common/BetterWiiMenu.h"
#include "Common/HttpRequest.h"
#include "UICommon/ForkRelease.h"

Updater::Updater(QWidget* parent, std::string track, std::string) : m_enabled(!track.empty())
{
  connect(this, &QThread::finished, this, &QObject::deleteLater);
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
    requestInterruption();
    wait();
  });
  connect(this, &Updater::Result, parent, [this, parent](const QString& tag, const QString& error) {
    if (!error.isEmpty())
    {
      if (m_manual)
        QMessageBox::warning(parent, tr("Fork update check"), error);
      return;
    }
    const auto remote = BetterWiiMenu::ParseVersion(tag.toStdString());
    const auto current = BetterWiiMenu::ParseVersion(BetterWiiMenu::VERSION);
    if (!remote || !current)
      return;
    if (*remote > *current)
    {
      QMessageBox box(QMessageBox::Information, tr("Better Wii Menu DE update"),
                      tr("Better Wii Menu DE %1 is available.\nYou are running %2.\n\n"
                         "Open the fork's releases page to download it?")
                          .arg(tag, QString::fromLatin1(BetterWiiMenu::VERSION)),
                      QMessageBox::Yes | QMessageBox::No, parent);
      box.setTextFormat(Qt::PlainText);
      if (box.exec() == QMessageBox::Yes)
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(BetterWiiMenu::RELEASES_URL)));
    }
    else if (m_manual)
      QMessageBox::information(parent, tr("Fork update check"),
                               tr("No newer stable Better Wii Menu DE release is available.\n"
                                  "This build: %1; latest published release: %2.")
                                   .arg(QString::fromLatin1(BetterWiiMenu::VERSION), tag));
  });
}
void Updater::CheckForUpdate()
{
  m_manual = true;
  start();
}
void Updater::run()
{
  if (!m_enabled && !m_manual)
    return;
  Common::HttpRequest request(std::chrono::seconds(10), [this](s64, s64 downloaded, s64, s64) {
    return !isInterruptionRequested() && downloaded < 1024 * 1024;
  });
  auto response =
      request.Get(BetterWiiMenu::RELEASES_API, {{"Accept", "application/vnd.github+json"},
                                                {"User-Agent", "Better-Wii-Menu-DE"}});
  if (!response)
  {
    emit Result({},
                tr("Could not check fork releases. Check your connection and try again later."));
    return;
  }
  const auto tag = UICommon::ParseForkRelease(
      std::string_view(reinterpret_cast<const char*>(response->data()), response->size()));
  if (!tag)
  {
    emit Result({}, tr("The fork release service returned an invalid response. Try again later."));
    return;
  }
  emit Result(QString::fromStdString(*tag), {});
}
