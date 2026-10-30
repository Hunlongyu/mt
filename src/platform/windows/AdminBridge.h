#pragma once
#include "DesktopServices.h"
#include "modules/gestures/GestureRules.h"
#include <functional>
#include <memory>

namespace mt {
class AdminBridge final : public QObject {
    Q_OBJECT
  public:
    using Completion = std::function<void(std::expected<void, QString>)>;
    explicit AdminBridge(QObject* parent = nullptr);
    ~AdminBridge() override;
    void start(Completion done, bool preview = false);
    // False means the child has not exited yet; retain its handle for retry.
    bool stop();
    bool ready() const;
    bool busy() const;
    QString status() const;
    void paste(WindowTarget target, QObject* context, Completion done);
    void suppressAltMenu(WindowTarget target, QObject* context, Completion done);
    void replayClick(WindowTarget target, quintptr foreground, QPoint position,
                     const QString& button);
    void gesture(const GestureAction&, WindowTarget, quintptr foreground, QObject* context,
                 Completion done);
    // Non-mutating authenticated round trip, also used in preview-only tests.
    void probe(QObject* context, Completion done);
  Q_SIGNALS:
    void changed();
    void failed(const QString& message);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
// Runs the same EXE as a bounded helper; no tray, hooks, data store or Shell API.
int runAdminHelper(const QString& pipe, quint32 parent, quint64 started, bool preview);
quint64 currentProcessStarted();
} // namespace mt
