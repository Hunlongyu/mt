#include <windows.h>

#include "EdgeWatcher.h"
#include "DesktopServices.h"
#include <shellscalingapi.h>

namespace mt {
namespace {
BOOL CALLBACK collect(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept {
    auto& out = *reinterpret_cast<QList<EdgeMonitor>*>(data);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        dpiX = 96;
    // RECT right/bottom are exclusive; QRect from corners keeps them exclusive by width.
    const auto& r = info.rcMonitor;
    try {
        out.append({QRect(r.left, r.top, r.right - r.left, r.bottom - r.top), dpiX / 96.0});
    } catch (...) {
    }
    return TRUE;
}
} // namespace
EdgeWatcher::EdgeWatcher(QObject* parent) : QObject(parent) {
    timer_.setInterval(30);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &EdgeWatcher::sample);
}
void EdgeWatcher::configure(const EdgeOptions& options) {
    options_ = options;
    dwell_.reset();
    processWindow_ = 0;
    if (!options.enabled || options.config.zones.isEmpty()) {
        timer_.stop();
        return;
    }
    clock_.start();
    monitorsAge_.invalidate();
    timer_.start();
}
void EdgeWatcher::refreshMonitors() {
    // Topology and DPI changes are rare; re-reading once a second is enough and
    // keeps monitor enumeration out of the 30 ms path.
    QList<EdgeMonitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &collect, reinterpret_cast<LPARAM>(&monitors));
    monitors_ = std::move(monitors);
    monitorsAge_.start();
}
bool EdgeWatcher::suppressed() {
    if (options_.suppressFullscreen && DesktopServices::foregroundFullscreen())
        return true;
    if (options_.blockList.isEmpty() && options_.allowList.isEmpty())
        return false;
    const HWND hwnd = GetForegroundWindow();
    // The desktop, the shell and this application are never filtered.
    if (!hwnd || hwnd == GetShellWindow() || hwnd == GetDesktopWindow())
        return false;
    const auto target = DesktopServices::foreground();
    if (target.processId == GetCurrentProcessId())
        return false;
    if (reinterpret_cast<quintptr>(hwnd) != processWindow_) {
        processWindow_ = reinterpret_cast<quintptr>(hwnd);
        processName_ = DesktopServices::processName(target);
    }
    return !edgeProcessAllowed(processName_, options_.blockList, options_.allowList);
}
void EdgeWatcher::sample() {
    try {
        if (!monitorsAge_.isValid() || monitorsAge_.elapsed() > 1000)
            refreshMonitors();
        POINT cursor{};
        if (!GetCursorPos(&cursor))
            return;
        auto zone = edgeZoneAt({cursor.x, cursor.y}, monitors_, options_.config);
        if (zone != EdgeZone::None && suppressed())
            zone = EdgeZone::None;
        if (const auto fired = dwell_.sample(zone, clock_.elapsed()); fired != EdgeZone::None)
            Q_EMIT triggered(fired);
    } catch (...) {
    }
}
} // namespace mt
