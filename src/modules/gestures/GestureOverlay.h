#pragma once
#include <QPolygon>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
namespace mt {
class Theme;
// Runtime trajectory surface only; never a management panel or input owner.
class GestureOverlay final : public QWidget {
  public:
    explicit GestureOverlay(Theme& theme);
    void present(const QPolygonF& desktopPoints, const QString& label, bool trail, bool matched = true);
    void finish();
    // Clear the backing surface while it is still visible, then hide it. A
    // subsequent show must never expose pixels belonging to the previous stroke.
    void dismiss();
  protected:
    void paintEvent(QPaintEvent*) override;
  private:
    Theme& theme_;
    QPolygonF points_;
    QString label_;
    bool trail_ = true;
    bool matched_ = true;
    QTimer fadeTimer_;
    QElapsedTimer fadeClock_;
    qreal opacity_ = 1;
};
}
