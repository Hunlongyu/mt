#include "EdgeTrigger.h"
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace mt {
namespace {
const QStringList keys{QStringLiteral("left"),        QStringLiteral("right"),
                       QStringLiteral("top"),         QStringLiteral("bottom"),
                       QStringLiteral("topLeft"),     QStringLiteral("topRight"),
                       QStringLiteral("bottomLeft"),  QStringLiteral("bottomRight")};
bool covered(QPoint p, const QList<EdgeMonitor>& monitors) {
    for (const auto& m : monitors)
        if (m.rect.contains(p))
            return true;
    return false;
}
} // namespace
QStringList edgeZoneKeys() {
    return keys;
}
EdgeZone edgeZoneFromKey(const QString& key) {
    const auto index = keys.indexOf(key);
    return index < 0 ? EdgeZone::None : static_cast<EdgeZone>(index + 1);
}
QString edgeZoneKey(EdgeZone zone) {
    return zone == EdgeZone::None ? QString{} : keys.value(static_cast<int>(zone) - 1);
}
EdgeZone edgeZoneAt(QPoint p, const QList<EdgeMonitor>& monitors, const EdgeConfig& config) {
    for (const auto& m : monitors) {
        const auto& r = m.rect;
        if (!r.contains(p))
            continue;
        const int t = std::max(1, static_cast<int>(std::lround(EdgeConfig::thicknessDip * m.scale)));
        const int c = std::max(t, static_cast<int>(std::lround(EdgeConfig::cornerDip * m.scale)));
        // QRect::right()/bottom() are inclusive; the last pixel column is right().
        const int fromLeft = p.x() - r.left();
        const int fromRight = r.right() - p.x();
        const int fromTop = p.y() - r.top();
        const int fromBottom = r.bottom() - p.y();
        // An edge is exposed when no other monitor continues past it at this point.
        const auto exposed = [&](int dx, int dy) {
            return !config.outerOnly || !covered(p + QPoint(dx, dy), monitors);
        };
        const bool left = fromLeft < t && exposed(-fromLeft - 1, 0);
        const bool right = fromRight < t && exposed(fromRight + 1, 0);
        const bool top = fromTop < t && exposed(0, -fromTop - 1);
        const bool bottom = fromBottom < t && exposed(0, fromBottom + 1);
        const auto enabled = [&](EdgeZone z) { return config.zones.contains(z); };
        struct Corner {
            EdgeZone zone;
            bool edgeH, edgeV; // touching the vertical / horizontal edge
            int distH, distV;  // distance to that edge
            int dxH, dyV;
        };
        const Corner corners[] = {
            {EdgeZone::TopLeft, left, top, fromLeft, fromTop, -fromLeft - 1, -fromTop - 1},
            {EdgeZone::TopRight, right, top, fromRight, fromTop, fromRight + 1, -fromTop - 1},
            {EdgeZone::BottomLeft, left, bottom, fromLeft, fromBottom, -fromLeft - 1,
             fromBottom + 1},
            {EdgeZone::BottomRight, right, bottom, fromRight, fromBottom, fromRight + 1,
             fromBottom + 1}};
        for (const auto& k : corners) {
            if (!enabled(k.zone))
                continue;
            // Within the corner square and touching at least one of its exposed
            // edges; the other edge must be exposed at the corner too.
            if (k.distH >= c || k.distV >= c)
                continue;
            const bool hExposed = exposed(k.dxH, 0);
            const bool vExposed = exposed(0, k.dyV);
            if (hExposed && vExposed && (k.edgeH || k.edgeV))
                return k.zone;
        }
        if (left && enabled(EdgeZone::Left))
            return EdgeZone::Left;
        if (right && enabled(EdgeZone::Right))
            return EdgeZone::Right;
        if (top && enabled(EdgeZone::Top))
            return EdgeZone::Top;
        if (bottom && enabled(EdgeZone::Bottom))
            return EdgeZone::Bottom;
        return EdgeZone::None;
    }
    return EdgeZone::None;
}
void EdgeDwell::reset() {
    *this = EdgeDwell{};
}
EdgeZone EdgeDwell::sample(EdgeZone zone, qint64 now) {
    if (now < last_) {
        // The clock went backwards: restart timing but never fire immediately.
        pending_ = EdgeZone::None;
        if (fired_ != EdgeZone::None)
            firedAt_ = now;
    }
    last_ = now;
    if (fired_ != EdgeZone::None) {
        if (zone != fired_)
            leftSinceFire_ = true;
        if (!leftSinceFire_ || now - firedAt_ < cooldownMs)
            return EdgeZone::None;
        fired_ = EdgeZone::None; // re-armed; this same sample may start a dwell
    }
    if (zone == EdgeZone::None) {
        pending_ = EdgeZone::None;
        return EdgeZone::None;
    }
    if (zone != pending_) {
        pending_ = zone;
        since_ = now;
        return EdgeZone::None;
    }
    if (now - since_ < dwellMs)
        return EdgeZone::None;
    fired_ = zone;
    firedAt_ = now;
    leftSinceFire_ = false;
    pending_ = EdgeZone::None;
    return zone;
}
bool edgeProcessAllowed(const QString& processName, const QStringList& blockList,
                        const QStringList& allowList) {
    const auto name = processName.trimmed();
    const auto listed = [&](const QStringList& list) {
        return !name.isEmpty() && list.contains(name, Qt::CaseInsensitive);
    };
    if (!allowList.isEmpty())
        return listed(allowList);
    return !listed(blockList);
}
QStringList parseProcessList(const QString& text) {
    static const QRegularExpression separators(QStringLiteral("[\\r\\n,;]+"));
    QStringList out;
    QSet<QString> seen;
    for (auto entry : text.split(separators, Qt::SkipEmptyParts)) {
        entry = entry.trimmed();
        if (entry.isEmpty() || seen.contains(entry.toCaseFolded()))
            continue;
        seen.insert(entry.toCaseFolded());
        out << entry;
    }
    return out;
}
} // namespace mt
