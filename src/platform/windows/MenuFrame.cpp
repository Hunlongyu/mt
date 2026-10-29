#include "MenuFrame.h"
#include <QCoreApplication>
#include <QOperatingSystemVersion>
#include <dwmapi.h>
#include <windows.h>

namespace mt {
MenuFrame::MenuFrame(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installNativeEventFilter(this);
}
MenuFrame::~MenuFrame() {
    QCoreApplication::instance()->removeNativeEventFilter(this);
}
bool MenuFrame::apply(QWindow& window, bool dark, bool smallCorners) {
    if (QOperatingSystemVersion::current() <
            QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22000) ||
        !window.handle())
        return false;
    const auto id = window.winId();
    const auto hwnd = reinterpret_cast<HWND>(id);
    // Qt temporarily uses WS_EX_LAYERED for the system menu fade animation.
    // A uniform-opacity fade is safe; a per-pixel alpha surface is not eligible
    // for DWM corners. Keep Qt's animation and prepare the frame before it ends.
    if (window.format().hasAlpha())
        return false;
    const auto oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (windows_.value(id) != &window) {
        windows_.insert(id, &window);
        connect(&window, &QObject::destroyed, this, [this, id, owner = &window] {
            if (!windows_.value(id) || windows_.value(id).data() == owner)
                windows_.remove(id);
        });
    }
    SetLastError(ERROR_SUCCESS);
    const bool styled = SetWindowLongPtrW(hwnd, GWL_STYLE, oldStyle | WS_THICKFRAME) != 0 ||
                        GetLastError() == ERROR_SUCCESS;
    const DWM_WINDOW_CORNER_PREFERENCE corner = smallCorners ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND;
    const MARGINS margins{1, 1, 1, 1};
    const bool framed = styled && SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins)) &&
                        SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                                        &corner, sizeof(corner)));
    if (!framed) {
        windows_.remove(id);
        SetWindowLongPtrW(hwnd, GWL_STYLE, oldStyle);
        const MARGINS none{};
        DwmExtendFrameIntoClientArea(hwnd, &none);
        return false;
    }
    const BOOL immersiveDark = dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersiveDark,
                          sizeof(immersiveDark));
    // Recompute the client frame without moving, activating, or changing popup grabs.
    return SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                            SWP_FRAMECHANGED | SWP_NOACTIVATE) != FALSE;
}
bool MenuFrame::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
    try {
        const auto* msg = static_cast<const MSG*>(message);
        if (!msg)
            return false;
        const auto id = reinterpret_cast<quintptr>(msg->hwnd);
        if (msg->message == WM_NCDESTROY) {
            windows_.remove(id);
            return false;
        }
        const auto window = windows_.value(id);
        if (!window || !window->handle() || window->winId() != id)
            return false;
        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            // Full-size Qt client area; DWM supplies the exterior frame and shadow.
            *result = 0;
            return true;
        }
    } catch (...) {
        // Never allow exceptions to cross the Windows callback boundary.
    }
    return false;
}
} // namespace mt
