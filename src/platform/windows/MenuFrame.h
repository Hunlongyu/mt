#pragma once
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QWindow>

namespace mt {
// Native frame policy only: DWM draws the rounded corners, border and system
// shadow of an opaque (non per-pixel-alpha) window. Actions, layout, input and
// painting remain in Qt. Used by menus (small corners) and panels (standard).
class MenuFrame final : public QObject, public QAbstractNativeEventFilter {
  public:
    explicit MenuFrame(QObject* parent = nullptr);
    ~MenuFrame() override;
    bool apply(QWindow& window, bool dark, bool smallCorners = true);
    bool nativeEventFilter(const QByteArray&, void* message, qintptr* result) override;

  private:
    QHash<quintptr, QPointer<QWindow>> windows_;
};
} // namespace mt
