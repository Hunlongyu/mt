#pragma once
#include "DesktopServices.h"
#include "modules/gestures/GestureRules.h"
#include <QVector>
#include <atomic>

namespace mt {
struct GestureInputOptions {
    bool enabled = false;
    QString trigger = QStringLiteral("right");
    int timeout = 1000;
    bool suppressFullscreen = true;
    GestureRules rules;
};
struct GestureFrame {
    quint64 generation = 0;
    quint64 cancellation = 0;
    quint64 stroke = 0;
    WindowTarget target;
    quintptr foregroundAtStart = 0;
    QString process;
    QVector<QPoint> points; // physical desktop coordinates
    bool drawing = false;
    bool finished = false;
    bool cancelled = false;
};
// Owns a dedicated Qt event loop for WH_MOUSE_LL / WH_KEYBOARD_LL. Callbacks
// perform bounded sampling and native admission; the timer publishes snapshots off-hook.
class GestureInput final : public QObject {
    Q_OBJECT
  public:
    explicit GestureInput(QObject* parent = nullptr);
    ~GestureInput() override;
    std::expected<void, QString> configure(const GestureInputOptions& options);
    void setAdminAvailable(bool available) noexcept {
        adminAvailable_.store(available);
    }
    // Only the explicitly registered launcher may receive gestures in this
    // process. Changing/hiding its native window invalidates in-flight work.
    void setLauncherWindow(quintptr handle) noexcept {
        if (launcherWindow_.exchange(handle) != handle)
            ++cancellation_;
    }
    void setBlocked(bool blocked) noexcept {
        if (blocked_.exchange(blocked) != blocked && blocked)
            ++cancellation_;
    }
    quint64 generation() const noexcept { return generation_; }
    quint64 cancellation() const noexcept { return cancellation_.load(); }
    quint64 stroke() const noexcept { return stroke_.load(); }
    static QPointF logicalPoint(QPoint physical);
    static bool isCurrent(const GestureFrame& frame);
  Q_SIGNALS:
    void frame(const mt::GestureFrame& frame);
    void failed(const QString& error);
    void clickReplayRequested(mt::WindowTarget target, quintptr foreground, QPoint position,
                              const QString& button, quint64 generation, quint64 cancellation);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool blocked_{false};
    std::atomic_bool adminAvailable_{false};
    std::atomic<quintptr> launcherWindow_{0};
    std::atomic<quint64> cancellation_{0};
    std::atomic<quint64> stroke_{0};
    quint64 generation_ = 0;
};
} // namespace mt
