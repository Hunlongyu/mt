#pragma once
#include "LauncherService.h"
#include "ui/components/Panel.h"
#include <QHash>
#include <QIcon>
#include <QSet>
class QGridLayout;
class QLineEdit;
class QListWidget;
class QTabBar;
class QToolButton;
class QScrollArea;
namespace mt {
class Theme;
class LauncherPanel final : public Panel {
    Q_OBJECT
  public:
    LauncherPanel(Theme& theme, LauncherService& service);
    void setPinned(bool pinned);
    // Rebuilds the tiles for a grid size; the window size follows. Columns must
    // match the catalog's layout, which the service migrates first.
    void setGrid(int columns, int rows);
    int gridColumns() const {
        return columns_;
    }
    int gridRows() const {
        return rows_;
    }
    void refresh();
    void retranslate();
    void fitToWorkArea(const QRect& area);
    void showNear(QPoint anchor, bool activate = true);
    void acceptIcon(const QString& id, const QString& source, const QImage& image);
  Q_SIGNALS:
    void launchRequested(mt::LaunchItem item);
    void revealRequested(const QString& target);
    void importRequested(QStringList paths, QString category, int slot);
    void settingsRequested();
    void pinnedChanged(bool pinned);
    void iconRequested(const QString& id, const QString& source);

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void hideEvent(QHideEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;

  private:
    QString categoryId() const;
    int absoluteSlot(int index) const {
        return page_ * pageSize() + index;
    }
    int pageSize() const {
        return columns_ * rows_;
    }
    QString itemAt(int index) const;
    void contextMenu(int index, QPoint point);
    void addItem();
    void editItem(int index);
    void categoryMenu(QPoint point);
    void changeCategory(int direction);
    void showSearch(const QString& initial = {});
    void updateSearch(bool preserveSelection = false);
    void positionSearch();
    // Mirrors the tab bar's own scroll state onto the chevrons at either end.
    void updateTabArrows();
    void syncTabs();
    void finishTabInteraction();
    void commitTabOrder();
    void applyTileStyle();
    void focusSelected();
    void changePage(int direction);
    int lastPage() const {
        return (LauncherCatalog::maxSlots - 1) / pageSize();
    }
    int visibleSlotCount() const;
    void activateItem(const QString& id);
    // Launches a (possibly adjusted) copy of an item and applies auto-hide.
    void launchCopy(const LaunchItem& item);
    void deleteItem(int index);
    void launchCategory(const QString& category);
    void ensureIcon(const LaunchItem& item);
    void checkFocusLater();
    Theme& theme_;
    LauncherService& service_;
    QList<QToolButton*> tiles_;
    QWidget* gridWidget_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QScrollArea* gridScroll_ = nullptr;
    QSize desiredSize_;
    int columns_ = 0;
    int rows_ = 0;
    QToolButton* pin_;
    QToolButton* menuButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    QTabBar* tabs_;
    QToolButton* tabLeft_ = nullptr;
    QToolButton* tabRight_ = nullptr;
    // QTabBar's built-in scroll buttons: they keep doing the scrolling, but
    // Qt puts both at the right end, so they are shrunk away and driven from
    // our chevrons instead.
    QToolButton* qtLeft_ = nullptr;
    QToolButton* qtRight_ = nullptr;
    QWidget* searchPopup_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* results_ = nullptr;
    QHash<QString, QPair<QString, QIcon>> icons_;
    QHash<QString, QString> pendingIcons_;
    bool iconsReady_ = false;
    QString displayedCategory_;
    QString persistedCategory_;
    quint64 selectionGeneration_ = 0;
    bool selectionPending_ = false;
    bool tabInteraction_ = false;
    quint64 tabInteractionGeneration_ = 0;
    bool tabOrderDirty_ = false;
    bool tabOrderPending_ = false;
    quint64 tabOrderGeneration_ = 0;
    int page_ = 0;
    int selected_ = 0;
    QPoint dragStart_;
};
} // namespace mt
