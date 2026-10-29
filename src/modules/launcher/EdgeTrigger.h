#pragma once
#include <QList>
#include <QRect>
#include <QStringList>

namespace mt {
// Screen-edge activation for the launcher: pure geometry and dwell logic, so the
// platform layer only samples the cursor and the foreground window.
enum class EdgeZone {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};
// Settings key for each zone ("left", "topLeft", ...), in enum order without None.
QStringList edgeZoneKeys();
EdgeZone edgeZoneFromKey(const QString& key);
QString edgeZoneKey(EdgeZone zone);

struct EdgeMonitor {
    QRect rect;        // physical pixels
    qreal scale = 1.0; // monitor DPI / 96
};
struct EdgeConfig {
    QList<EdgeZone> zones;
    bool outerOnly = true; // only edges of the whole desktop, not seams between monitors
    static constexpr int thicknessDip = 4;
    static constexpr int cornerDip = 16;
};
// Corners win over edges; one sample matches at most one zone.
EdgeZone edgeZoneAt(QPoint cursor, const QList<EdgeMonitor>& monitors, const EdgeConfig& config);

// Idle → Pending (in a zone) → Triggered after dwelling. After firing, the zone
// re-arms once the cursor has left it and the cooldown has passed; a cursor
// that already came back starts a new dwell on that same sample.
class EdgeDwell {
  public:
    static constexpr qint64 dwellMs = 180;
    static constexpr qint64 cooldownMs = 100;
    // Pass EdgeZone::None while activation is suppressed. Returns the zone that
    // fired on this sample, or None.
    EdgeZone sample(EdgeZone zone, qint64 nowMs);
    void reset();

  private:
    EdgeZone pending_ = EdgeZone::None;
    qint64 since_ = 0;
    EdgeZone fired_ = EdgeZone::None;
    qint64 firedAt_ = 0;
    bool leftSinceFire_ = false;
    qint64 last_ = 0;
};

// Foreground process filter: names are matched case-insensitively; a non-empty
// allow list admits only its entries and wins over the block list. An unknown
// name passes a pure block list but not an allow list.
bool edgeProcessAllowed(const QString& processName, const QStringList& blockList,
                        const QStringList& allowList);
// One executable name per line; commas and semicolons also separate entries.
QStringList parseProcessList(const QString& text);
} // namespace mt
