#pragma once
#include "ClipboardRecord.h"
#include "ui/components/Panel.h"
#include <QPointer>
class QListView;
class QLineEdit;
class QGraphicsOpacityEffect;
class QVariantAnimation;
class QToolButton;
class QAbstractListModel;
class QPlainTextEdit;
class QTimer;
namespace mt {
class Theme;
class ClipboardPreview;
// User-configurable panel behaviour (the clipboard page of the settings).
struct ClipboardBehavior {
    bool hoverPreview = true;
    bool spacePreview = true;
    bool previewLeft = true;
    int openKey = Qt::Key_O;                    // 0 disables the key
    Qt::MouseButton openButton = Qt::MiddleButton; // Qt::NoButton disables it
};
class ClipboardPanel final : public Panel {
    Q_OBJECT
  public:
    static constexpr int searchMotionMs = 200;
    explicit ClipboardPanel(Theme& theme, bool animateSearch = true);
    void setRecords(ClipboardRecords records);
    // The next record list (the refresh after opening) selects the first row.
    void selectNewestOnNextRecords();
    void setMaskSensitive(bool enabled);
    void setBehavior(const ClipboardBehavior& behavior);
    // One mouse side-button press: +1 scrolls the preview down, -1 up, with momentum.
    void sideScroll(int direction);
    void showSearch();
    void closeSearch();
    void handleKey(int key, bool shift = false, bool control = false, bool down = true);
    void performAction(int row, int action);
    bool searching() const;
    // -1 when no filter is selected; kept across openings like the original.
    int filter() const {
        return filter_;
    }
    void setPaused(bool paused);
    void displayPreview(const ClipboardRecord& record, quint64 token);
  Q_SIGNALS:
    void queryChanged(const QString& query, int filter);
    void pasteRequested(mt::ClipboardRecord record, bool plainText);
    void toggleFlagRequested(const QString& id, mt::ClipFlag flag);
    void removeRequested(const QString& id);
    void settingsRequested();
    void pauseRequested(bool paused);
    void searchModeChanged(bool searching);
    void openRequested(const QString& target);
    void previewRequested(const QString& id, quint64 token);
    void previewVisibleChanged(bool visible);

  protected:
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    ClipboardRecord selected() const;
    void setFilter(int filter);
    void showPreview(int row);
    void applySearchProgress(qreal t);
    void animateSearchTo(qreal target);
    void hidePreview();
    void hoverAt(QPoint viewportPos);
    void showHelp();
    void closeHelp();
    QString openLabel() const;
    Theme& theme_;
    QListView* list_ = nullptr;
    QWidget* help_ = nullptr;
    QPlainTextEdit* helpText_ = nullptr;
    QPointer<QWidget> listViewport_;
    QAbstractListModel* model_ = nullptr;
    QWidget* toolbar_ = nullptr;
    QWidget* browse_ = nullptr;
    QWidget* searchBox_ = nullptr;
    QToolButton* searchButton_ = nullptr;
    QGraphicsOpacityEffect* browseFade_ = nullptr;
    QGraphicsOpacityEffect* searchFade_ = nullptr;
    QVariantAnimation* searchAnimation_ = nullptr;
    int browseWidth_ = 0;
    qreal searchProgress_ = 0.0;
    bool searching_ = false;
    bool animateSearch_ = true;
    QLineEdit* search_ = nullptr;
    QList<QToolButton*> filters_;
    ClipboardRecords records_;
    ClipboardPreview* preview_ = nullptr;
    QTimer* hoverTimer_ = nullptr;
    int hoverRow_ = -1;
    bool spaceHeld_ = false;
    bool selectNewest_ = false;
    int filter_ = -1;
    bool masked_ = true;
    bool paused_ = false;
    int savedScroll_ = 0;
    QString savedSelectionId_;
    int savedSelectionRow_ = 0;
    bool restoreBrowse_ = false;
    quint64 previewToken_ = 0;
    QString previewTargetId_;
    ClipboardBehavior behavior_;
    QTimer* sideTimer_ = nullptr;
    qreal sideVelocity_ = 0.0;
    qreal sideCarry_ = 0.0;
    int pressedAction_ = -1;
    QString pressedActionId_;
};
} // namespace mt
