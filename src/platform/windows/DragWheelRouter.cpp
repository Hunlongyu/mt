#include <windows.h>
#include <windowsx.h>

#include "DragWheelRouter.h"
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <utility>
#include <wil/resource.h>

namespace mt {
struct DragWheelRouter::Impl {
    static thread_local Impl* current;
    wil::unique_hhook hook;
    QPointer<QWindow> window;
    HWND handle = nullptr;
    QTimer delivery;
    int remainder = 0;
    int pending = 0;

    bool accepts(const MSG& message) const {
        if (!window || !window->isVisible() || reinterpret_cast<HWND>(window->winId()) != handle ||
            !IsWindowVisible(handle) || !(GET_KEYSTATE_WPARAM(message.wParam) & MK_LBUTTON))
            return false;
        POINT point{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
        RECT client{};
        if (!GetClientRect(handle, &client) || !ScreenToClient(handle, &point))
            return false;
        return PtInRect(&client, point);
    }

    static LRESULT CALLBACK messages(int code, WPARAM removed, LPARAM data) noexcept {
        if (code == HC_ACTION && removed == PM_REMOVE && current) {
            auto& message = *reinterpret_cast<MSG*>(data);
            if (message.message == WM_MOUSEWHEEL) {
                try {
                    auto& self = *current;
                    if (self.accepts(message)) {
                        // Only collect input here. Refresh and asynchronous persistence
                        // run later on the Qt event loop, outside this native callback.
                        if (!self.delivery.isActive())
                            self.delivery.start();
                        self.remainder += GET_WHEEL_DELTA_WPARAM(message.wParam);
                        self.pending -= self.remainder / WHEEL_DELTA;
                        self.remainder %= WHEEL_DELTA;
                        // Do not let the same wheel also scroll the grid or QTabBar.
                        message.message = WM_NULL;
                        message.wParam = 0;
                        message.lParam = 0;
                    } else {
                        self.remainder = 0;
                    }
                } catch (...) {
                    // Never unwind through Windows' native callback boundary.
                }
            }
        }
        return CallNextHookEx(nullptr, code, removed, data);
    }
};
thread_local DragWheelRouter::Impl* DragWheelRouter::Impl::current = nullptr;

DragWheelRouter::DragWheelRouter(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->delivery.setSingleShot(true);
    impl_->delivery.setInterval(0);
    connect(&impl_->delivery, &QTimer::timeout, this, &DragWheelRouter::flush);
}
DragWheelRouter::~DragWheelRouter() {
    stop();
}
bool DragWheelRouter::start(QWindow& window) {
    stop();
    if (Impl::current)
        return false;
    impl_->window = &window;
    impl_->handle = reinterpret_cast<HWND>(window.winId());
    impl_->hook.reset(SetWindowsHookExW(WH_GETMESSAGE, &Impl::messages, nullptr, GetCurrentThreadId()));
    if (!impl_->hook) {
        stop();
        return false;
    }
    Impl::current = impl_.get();
    return true;
}
void DragWheelRouter::stop() {
    if (Impl::current == impl_.get())
        Impl::current = nullptr;
    impl_->hook.reset();
    impl_->delivery.stop();
    impl_->window.clear();
    impl_->handle = nullptr;
    impl_->remainder = 0;
    impl_->pending = 0;
}
void DragWheelRouter::flush() {
    impl_->delivery.stop();
    const int steps = std::exchange(impl_->pending, 0);
    if (steps && impl_->window && impl_->window->isVisible())
        Q_EMIT stepped(steps);
}
} // namespace mt
