#pragma once
#include "modules/launcher/EdgeTrigger.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace mt {
struct EdgeOptions {
    bool enabled = false;
    EdgeConfig config;
    bool suppressFullscreen = true;
    QStringList blockList;
    QStringList allowList;
};
// Samples the cursor every 30 ms while edge activation is enabled. Sampling only
// reads the cursor and cached monitor geometry; the foreground process name is
// looked up only while the cursor is in a hot zone and the foreground window
// changed. No hooks, no window enumeration.
class EdgeWatcher final : public QObject {
    Q_OBJECT
  public:
    explicit EdgeWatcher(QObject* parent = nullptr);
    void configure(const EdgeOptions& options);
  Q_SIGNALS:
    void triggered(mt::EdgeZone zone);

  private:
    void sample();
    void refreshMonitors();
    bool suppressed();
    EdgeOptions options_;
    QTimer timer_;
    QElapsedTimer clock_;
    QElapsedTimer monitorsAge_;
    QList<EdgeMonitor> monitors_;
    EdgeDwell dwell_;
    quintptr processWindow_ = 0;
    QString processName_;
};
} // namespace mt
