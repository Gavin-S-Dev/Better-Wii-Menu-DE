// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameList/GridProxyModel.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include "Common/FileUtil.h"

#include "DolphinQt/GameList/GameListModel.h"

#include "Core/Config/UISettings.h"

#include "UICommon/GameFile.h"

const QSize LARGE_BANNER_SIZE(144, 48);

GridProxyModel::GridProxyModel(QObject* parent) : QSortFilterProxyModel(parent)
{
  setDynamicSortFilter(true);
}

QVariant GridProxyModel::data(const QModelIndex& i, int role) const
{
  QModelIndex source_index = mapToSource(i);
  if (role == Qt::DisplayRole)
  {
    return sourceModel()->data(
        sourceModel()->index(source_index.row(), static_cast<int>(GameListModel::Column::Title)),
        Qt::DisplayRole);
  }
  else if (role == Qt::DecorationRole)
  {
    auto* model = static_cast<GameListModel*>(sourceModel());

    if (model->GetGameFile(source_index.row())->IsWiiSystemMenu())
    {
      const bool cover = Config::Get(Config::MAIN_USE_GAME_COVERS);
      const auto path = File::GetSysDirectory() +
                        (cover ? "Resources/wii-menu-cover.svg" : "Resources/wii-menu-banner.svg");
      return QPixmap(QString::fromStdString(path))
          .scaled((cover ? QSize(160, 224) : LARGE_BANNER_SIZE) * model->GetScale(),
                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const auto& buffer = model->GetGameFile(source_index.row())->GetCoverImage().buffer;

    QSize size = Config::Get(Config::MAIN_USE_GAME_COVERS) ? QSize(160, 224) : LARGE_BANNER_SIZE;
    QPixmap pixmap(size * model->GetScale() * QPixmap().devicePixelRatio());

    if (buffer.empty() || !Config::Get(Config::MAIN_USE_GAME_COVERS))
    {
      QPixmap banner = model
                           ->data(model->index(source_index.row(),
                                               static_cast<int>(GameListModel::Column::Banner)),
                                  Qt::DecorationRole)
                           .value<QPixmap>();

      banner = banner.scaled(pixmap.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

      pixmap.fill();

      QPainter painter(&pixmap);

      painter.drawPixmap(0, pixmap.height() / 2 - banner.height() / 2, banner.width(),
                         banner.height(), banner);

      return pixmap;
    }
    else
    {
      pixmap = QPixmap::fromImage(QImage::fromData(
          reinterpret_cast<const unsigned char*>(&buffer[0]), static_cast<int>(buffer.size())));

      return pixmap.scaled(QSize(160, 224) * model->GetScale() * pixmap.devicePixelRatio(),
                           Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
  }
  if (role == Qt::ToolTipRole)
    return sourceModel()->data(source_index, role);
  return QVariant();
}

bool GridProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
{
  GameListModel* glm = qobject_cast<GameListModel*>(sourceModel());
  return glm->ShouldDisplayGameListItem(source_row);
}

bool GridProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
  const bool left_menu = left.data(GameListModel::WII_MENU_ROLE).toBool();
  const bool right_menu = right.data(GameListModel::WII_MENU_ROLE).toBool();
  if (left_menu != right_menu)
    return sortOrder() == Qt::AscendingOrder ? left_menu : right_menu;
  if (left.data(GameListModel::SORT_ROLE) != right.data(GameListModel::SORT_ROLE))
    return QSortFilterProxyModel::lessThan(left, right);

  // If two items are otherwise equal, compare them by their title
  const auto right_title = sourceModel()
                               ->index(right.row(), static_cast<int>(GameListModel::Column::Title))
                               .data()
                               .toString();
  const auto left_title = sourceModel()
                              ->index(left.row(), static_cast<int>(GameListModel::Column::Title))
                              .data()
                              .toString();

  if (sortOrder() == Qt::AscendingOrder)
    return left_title < right_title;

  return right_title < left_title;
}
