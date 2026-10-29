#pragma once
#include <QSet>
#include <memory>
#include <QWidget>
class QVBoxLayout;
class QVariantAnimation;
namespace mt {
class Theme;
class MenuFrame;
class Panel : public QWidget {
    Q_OBJECT
  public:
    explicit Panel(Theme& theme, const QString& accessibleTitle, QWidget* parent = nullptr);
    ~Panel() override;
    // Re-applies the system frame. Call after changing native window flags,
    // which makes Qt rebuild the window style.
    void refreshFrame();
    QVBoxLayout* layoutColumn() const {
        return column_;
    }
    void showNear(QPoint anchor, bool activate = true);
    void setPanelOpacity(int percent);
    void enableTitleDrag(QWidget* title);
    // Fade in on show. Disabled by default so tests and reduced-motion hosts
    // see the final opacity immediately.
    void setAnimated(bool animated) {
        animated_ = animated;
    }
    qreal panelOpacity() const {
        return opacity_;
    }
    bool keepOpen = false;
  Q_SIGNALS:
    void dismissed();
    void opacityChanged(qreal value);

  protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    void applyOpacity(qreal value);
    QVBoxLayout* column_;
    Theme& theme_;
    std::unique_ptr<MenuFrame> frame_;
    QVariantAnimation* fade_ = nullptr;
    qreal opacity_ = 1.0;
    bool animated_ = false;
    QPoint dragOffset_;
    bool dragging_ = false;
    QSet<QObject*> dragWidgets_;
};
} // namespace mt
