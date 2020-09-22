#pragma once
#include <QtWidgets/QListView>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableView>

class GameList;
struct GameListEntry;

class GameListModel;
class GameListSortModel;

class QtHostInterface;

class GameListWidget : public QStackedWidget
{
  Q_OBJECT

public:
  GameListWidget(QWidget* parent = nullptr);
  ~GameListWidget();

  void initialize(QtHostInterface* host_interface);

Q_SIGNALS:
  void entrySelected(const GameListEntry* entry);
  void entryDoubleClicked(const GameListEntry* entry);
  void entryContextMenuRequested(const QPoint& point, const GameListEntry* entry);

private Q_SLOTS:
  void onGameListRefreshed();
  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
  void onTableViewItemDoubleClicked(const QModelIndex& index);
  void onTableViewContextMenuRequested(const QPoint& point);
  void onTableViewHeaderContextMenuRequested(const QPoint& point);
  void onTableViewHeaderSortIndicatorChanged(int, Qt::SortOrder);
  void listZoomIn();
  void listZoomOut();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  const GameListEntry* getSelectedEntry() const;
  void resizeTableViewColumnsToFit();
  void loadTableViewColumnVisibilitySettings();
  void saveTableViewColumnVisibilitySettings();
  void saveTableViewColumnVisibilitySettings(int column);
  void loadTableViewColumnSortSettings();
  void saveTableViewColumnSortSettings();
  void listZoom(float delta);

  QtHostInterface* m_host_interface = nullptr;
  GameList* m_game_list = nullptr;

  GameListModel* m_model = nullptr;
  GameListSortModel* m_table_sort_model = nullptr;
  GameListSortModel* m_list_sort_model = nullptr;
  QTableView* m_table_view = nullptr;
  QListView* m_list_view = nullptr;
};
