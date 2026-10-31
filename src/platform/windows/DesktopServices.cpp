#include <windows.h>
#include "infrastructure/i18n/I18n.h"

#include "DesktopServices.h"
#include "InputTag.h"
#include "CaretLocator.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <QUrl>
#include <array>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
#include <wil/com.h>
#include <wil/resource.h>

namespace mt {
namespace {
struct Key {
    UINT modifiers = 0;
    UINT vk = 0;
};
std::expected<Key, QString> parseKey(const QString& text) {
    const QKeySequence seq = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (seq.isEmpty() || seq.count() != 1)
        return std::unexpected(tx(u"无效快捷键：%1").arg(text));
    const auto key = seq[0];
    Key out;
    const auto mods = key.keyboardModifiers();
    if (mods & Qt::ControlModifier)
        out.modifiers |= MOD_CONTROL;
    if (mods & Qt::AltModifier)
        out.modifiers |= MOD_ALT;
    if (mods & Qt::ShiftModifier)
        out.modifiers |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
        out.modifiers |= MOD_WIN;
    const auto k = key.key();
    if ((k >= Qt::Key_A && k <= Qt::Key_Z) || (k >= Qt::Key_0 && k <= Qt::Key_9))
        out.vk = static_cast<UINT>(k);
    else if (k >= Qt::Key_F1 && k <= Qt::Key_F24)
        out.vk = VK_F1 + static_cast<UINT>(k - Qt::Key_F1);
    else {
        switch (k) {
        case Qt::Key_Space:
            out.vk = VK_SPACE;
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            out.vk = VK_RETURN;
            break;
        case Qt::Key_Tab:
            out.vk = VK_TAB;
            break;
        case Qt::Key_Escape:
            out.vk = VK_ESCAPE;
            break;
        case Qt::Key_Up:
            out.vk = VK_UP;
            break;
        case Qt::Key_Down:
            out.vk = VK_DOWN;
            break;
        case Qt::Key_Left:
            out.vk = VK_LEFT;
            break;
        case Qt::Key_Right:
            out.vk = VK_RIGHT;
            break;
        case Qt::Key_Home:
            out.vk = VK_HOME;
            break;
        case Qt::Key_End:
            out.vk = VK_END;
            break;
        case Qt::Key_Delete:
            out.vk = VK_DELETE;
            break;
        case Qt::Key_Insert:
            out.vk = VK_INSERT;
            break;
        default:
            return std::unexpected(tx(u"暂不支持此快捷键：%1").arg(text));
        }
    }
    if (out.modifiers == 0)
        return std::unexpected(tx(u"全局快捷键须包含 Ctrl、Alt、Shift 或 Win"));
    return out;
}
HWND window(WindowTarget target) {
    return reinterpret_cast<HWND>(target.handle);
}
quint64 processStart(DWORD pid) {
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process.get(), &created, &exit, &kernel, &user))
        return 0;
    return (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
QString expand(const QString& input) {
    const auto wide = input.toStdWString();
    const DWORD size = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (!size)
        return input;
    std::wstring output(size, L'\0');
    if (!ExpandEnvironmentStringsW(wide.c_str(), output.data(), size))
        return input;
    output.resize(size - 1);
    return QString::fromStdWString(output);
}
QString resolvedPath(const QString& input, const QString& base) {
    auto result = expand(input);
    if (!QUrl(result).scheme().isEmpty() && !QDir::isAbsolutePath(result))
        return result;
    if (QDir::isRelativePath(result))
        result = QDir(base).absoluteFilePath(result);
    return QDir::toNativeSeparators(result);
}
} // namespace
struct DesktopServices::Impl {
    DesktopServices* owner;
    wil::unique_handle mutex;
    wil::unique_hhook navigationHook;
    wil::unique_hhook mouseHook;
    std::unique_ptr<CaretLocator> caret;
    bool sideCapture = false;
    int sideHeld = 0; // a swallowed side-button press whose release must be swallowed too
    HWND hwnd = nullptr;
    bool primary = false;
    std::wstring className;
    QMap<int, Key> keys;
    std::array<bool, 256> capturedKeys{};
    static inline Impl* hookOwner = nullptr;
    static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        try {
            if (self && msg == WM_HOTKEY) {
                const int id = static_cast<int>(wParam);
                QMetaObject::invokeMethod(
                    self->owner, [owner = self->owner, id] { Q_EMIT owner->hotkey(id); },
                    Qt::QueuedConnection);
                return 0;
            }
            if (self && msg == WM_APP + 1) {
                QMetaObject::invokeMethod(
                    self->owner, [owner = self->owner] { Q_EMIT owner->activated(); },
                    Qt::QueuedConnection);
                return 0;
            }
        } catch (...) {
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    static LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM data) noexcept {
        auto* self = hookOwner;
        if (code != HC_ACTION || !self)
            return CallNextHookEx(nullptr, code, message, data);
        const auto& e = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        // Skip only the paste keystrokes this process injects itself; other
        // injected input (remappers, UI automation) still drives the panel.
        if ((e.flags & LLKHF_INJECTED) && e.dwExtraInfo == ownInputTag)
            return CallNextHookEx(nullptr, code, message, data);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const auto vk = e.vkCode;
        if (vk >= self->capturedKeys.size())
            return CallNextHookEx(nullptr, code, message, data);
        if (!down && self->capturedKeys[vk]) {
            self->capturedKeys[vk] = false;
            if (vk == VK_SPACE)
                QMetaObject::invokeMethod(
                    self->owner,
                    [owner = self->owner] {
                        Q_EMIT owner->panelKey(Qt::Key_Space, false, false, false);
                    },
                    Qt::QueuedConnection);
            return 1;
        }
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        if (foregroundPid == GetCurrentProcessId())
            return CallNextHookEx(nullptr, code, message, data);
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        int key = 0;
        switch (vk) {
        case VK_UP:
            key = Qt::Key_Up;
            break;
        case VK_DOWN:
            key = Qt::Key_Down;
            break;
        case VK_RETURN:
            key = Qt::Key_Return;
            break;
        case VK_ESCAPE:
            key = Qt::Key_Escape;
            break;
        case VK_DELETE:
            key = Qt::Key_Delete;
            break;
        case VK_SPACE:
            key = Qt::Key_Space;
            break;
        case VK_HOME:
            key = Qt::Key_Home;
            break;
        case VK_END:
            key = Qt::Key_End;
            break;
        case VK_LEFT:
            key = Qt::Key_Left;
            break;
        case VK_RIGHT:
            key = Qt::Key_Right;
            break;
        case VK_TAB:
            key = Qt::Key_Tab;
            break;
        case VK_OEM_2:
            key = Qt::Key_Slash;
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
            key = static_cast<int>(vk);
            break;
        default:
            // Letters drive in-panel actions and the configurable open key. Ctrl
            // chords pass through except the panel's own Ctrl+D / Ctrl+F / Ctrl+P.
            if (vk >= 'A' && vk <= 'Z' && (!ctrl || vk == 'D' || vk == 'F' || vk == 'P'))
                key = Qt::Key_A + static_cast<int>(vk - 'A');
            break;
        }
        if (down && key && !alt) {
            try {
                QMetaObject::invokeMethod(
                    self->owner,
                    [owner = self->owner, key, shift, ctrl] {
                        Q_EMIT owner->panelKey(key, shift, ctrl, true);
                    },
                    Qt::QueuedConnection);
                self->capturedKeys[vk] = true;
                return 1;
            } catch (...) {
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) noexcept {
        auto* self = hookOwner;
        if (code != HC_ACTION || !self)
            return CallNextHookEx(nullptr, code, message, data);
        try {
            const auto& e = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
            if (e.dwExtraInfo == ownInputTag)
                return CallNextHookEx(nullptr, code, message, data);
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN) {
                DWORD pid = 0;
                GetWindowThreadProcessId(WindowFromPoint(e.pt), &pid);
                if (pid != GetCurrentProcessId())
                    QMetaObject::invokeMethod(
                        self->owner, [owner = self->owner] { Q_EMIT owner->outsidePress(); },
                        Qt::QueuedConnection);
            }
            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) {
                const auto button = HIWORD(e.mouseData);
                const int direction = button == XBUTTON1 ? 1 : button == XBUTTON2 ? -1 : 0;
                if (direction && message == WM_XBUTTONDOWN && self->sideCapture) {
                    self->sideHeld = direction;
                    QMetaObject::invokeMethod(
                        self->owner,
                        [owner = self->owner, direction] { Q_EMIT owner->sideButton(direction); },
                        Qt::QueuedConnection);
                    return 1;
                }
                // Finish a swallowed press even if the preview closed meanwhile, so
                // the release never reaches the window underneath.
                if (direction && message == WM_XBUTTONUP && self->sideHeld == direction) {
                    self->sideHeld = 0;
                    return 1;
                }
            }
        } catch (...) {
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    ~Impl() {
        mouseHook.reset();
        navigationHook.reset();
        if (hookOwner == this)
            hookOwner = nullptr;
        if (hwnd) {
            for (auto it = keys.cbegin(); it != keys.cend(); ++it)
                UnregisterHotKey(hwnd, it.key());
            DestroyWindow(hwnd);
        }
        if (primary)
            UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
    }
};
DesktopServices::DesktopServices(const QString& identity, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    const auto hash = QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    impl_->className = (QStringLiteral("mt.Development.") + hash).toStdWString();
    const auto mutexName = (QStringLiteral("Local\\mt.Development.") + hash).toStdWString();
    impl_->mutex.reset(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!impl_->mutex)
        return;
    impl_->primary = GetLastError() != ERROR_ALREADY_EXISTS;
    if (!impl_->primary)
        return;
    WNDCLASSW cls{};
    cls.lpfnWndProc = &Impl::procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = impl_->className.c_str();
    if (!RegisterClassW(&cls))
        return;
    impl_->hwnd = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  cls.hInstance, impl_.get());
}
DesktopServices::~DesktopServices() = default;
bool DesktopServices::primaryInstance() const {
    return impl_->primary;
}
bool DesktopServices::valid() const {
    return impl_->mutex && (!impl_->primary || impl_->hwnd);
}
bool DesktopServices::notifyPrimary() const {
    const auto other = FindWindowExW(HWND_MESSAGE, nullptr, impl_->className.c_str(), nullptr);
    return other && PostMessageW(other, WM_APP + 1, 0, 0);
}
std::expected<void, QString> DesktopServices::setHotkeys(const QMap<int, QString>& keys,
                                                        bool bestEffort) {
    QMap<int, Key> next;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        auto parsed = parseKey(it.value());
        if (!parsed)
            return std::unexpected(parsed.error());
        for (const auto& existing : std::as_const(next))
            if (existing.modifiers == parsed->modifiers && existing.vk == parsed->vk)
                return std::unexpected(tx(u"全局快捷键重复"));
        next.insert(it.key(), *parsed);
    }
    for (auto it = impl_->keys.cbegin(); it != impl_->keys.cend(); ++it)
        UnregisterHotKey(impl_->hwnd, it.key());
    QList<int> registered;
    QStringList busy;
    for (auto it = next.cbegin(); it != next.cend(); ++it) {
        if (!RegisterHotKey(impl_->hwnd, it.key(), it->modifiers | MOD_NOREPEAT, it->vk)) {
            const DWORD error = GetLastError();
            if (bestEffort) {
                busy.append(tx(u"%1（%2）").arg(keys[it.key()]).arg(error));
                continue;
            }
            for (int id : registered)
                UnregisterHotKey(impl_->hwnd, id);
            QString rollbackFailure;
            for (auto old = impl_->keys.cbegin(); old != impl_->keys.cend(); ++old)
                if (!RegisterHotKey(impl_->hwnd, old.key(), old->modifiers | MOD_NOREPEAT, old->vk))
                    rollbackFailure = tx(u"；原快捷键恢复失败，请重新设置");
            return std::unexpected(
                tx(u"快捷键 %1 注册失败（%2），可能已被其他程序占用%3")
                    .arg(keys[it.key()])
                    .arg(error)
                    .arg(rollbackFailure));
        }
        registered.append(it.key());
    }
    for (auto it = next.begin(); it != next.end();)
        it = registered.contains(it.key()) ? std::next(it) : next.erase(it);
    impl_->keys = next;
    if (!busy.isEmpty())
        return std::unexpected(tx(u"快捷键 %1 注册失败，可能已被其他程序占用；其余快捷键可用")
                                   .arg(busy.join(tx(u"、"))));
    return {};
}
QFont DesktopServices::systemUiFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    // Request 96-DPI metrics; Qt applies the window's scaling to logical font sizes.
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, 96)) {
        const auto& system = metrics.lfMessageFont;
        font.setFamily(QString::fromWCharArray(system.lfFaceName));
        if (system.lfHeight < 0)
            font.setPointSizeF(-system.lfHeight * 72.0 / 96.0);
        font.setItalic(system.lfItalic != 0);
        font.setWeight(QFont::Normal);
    }
    return font;
}
bool DesktopServices::clientAnimationsEnabled() {
    BOOL enabled = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled;
}
WindowTarget DesktopServices::foreground() {
    const HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!hwnd || pid == GetCurrentProcessId())
        return {};
    return windowTarget(reinterpret_cast<quintptr>(hwnd));
}
WindowTarget DesktopServices::windowTarget(quintptr handle) {
    const auto hwnd = reinterpret_cast<HWND>(handle);
    DWORD pid = 0;
    if (!hwnd || !IsWindow(hwnd) || !GetWindowThreadProcessId(hwnd, &pid)) return {};
    GUITHREADINFO info{sizeof(GUITHREADINFO)};
    GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info);
    return {reinterpret_cast<quintptr>(hwnd), pid, processStart(pid),
            reinterpret_cast<quintptr>(info.hwndFocus)};
}
bool DesktopServices::validTarget(WindowTarget t) {
    DWORD pid = 0;
    return t.handle && IsWindow(window(t)) && GetWindowThreadProcessId(window(t), &pid) &&
           pid == t.processId && (!t.processStarted || processStart(pid) == t.processStarted);
}
bool DesktopServices::isForeground(WindowTarget t) {
    if (!validTarget(t) || GetForegroundWindow() != window(t))
        return false;
    GUITHREADINFO info{sizeof(GUITHREADINFO)};
    return !t.focusedControl ||
           (GetGUIThreadInfo(GetWindowThreadProcessId(window(t), nullptr), &info) &&
            reinterpret_cast<quintptr>(info.hwndFocus) == t.focusedControl);
}
bool DesktopServices::activate(WindowTarget t) {
    return validTarget(t) && (isForeground(t) || SetForegroundWindow(window(t)));
}
QString DesktopServices::processName(WindowTarget t) {
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t.processId));
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!process || !QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return {};
    path.resize(size);
    return QFileInfo(QString::fromStdWString(path)).fileName();
}
bool DesktopServices::clipboardSensitive() {
    static const UINT marker =
        RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    return marker && IsClipboardFormatAvailable(marker);
}
quint32 DesktopServices::clipboardSequence() {
    return GetClipboardSequenceNumber();
}
bool DesktopServices::modifiersReleased() {
    return !((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
              GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) &
             0x8000);
}
void DesktopServices::enableCaretTracking() {
    if (!impl_->caret)
        impl_->caret = std::make_unique<CaretLocator>();
}
void DesktopServices::requestCaretRect(
    QObject* context, std::function<void(std::optional<QRect>, QString)> done) {
    enableCaretTracking();
    const auto foreground = reinterpret_cast<quintptr>(GetForegroundWindow());
    impl_->caret->request(foreground, context,
        [done = std::move(done)](std::optional<QRect> physical, QString trace) mutable {
            const auto logical = [&]() -> std::optional<QRect> {
                if (!physical)
                    return std::nullopt;
                const POINT center{physical->center().x(), physical->center().y()};
                MONITORINFOEXW monitor{};
                monitor.cbSize = sizeof(monitor);
                if (!GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor))
                    return std::nullopt;
                // Same physical offset within the monitor, scaled to that screen's logical size.
                // Match by HMONITOR: in Qt 6 QScreen::name() is the friendly monitor name,
                // not the GDI device name (DISPLAYn).
                const auto handle = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
                for (auto* screen : QGuiApplication::screens())
                    if (const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
                        native && native->handle() == handle) {
                        const auto origin = screen->geometry().topLeft();
                        const auto scale = screen->devicePixelRatio();
                        const auto map = [&](int x, int y) {
                            return origin + QPoint(qRound((x - monitor.rcMonitor.left) / scale),
                                                   qRound((y - monitor.rcMonitor.top) / scale));
                        };
                        return QRect(map(physical->left(), physical->top()),
                                     map(physical->right() + 1, physical->bottom() + 1) - QPoint(1, 1));
                    }
                return std::nullopt;

            }();
            done(logical, std::move(trace));
        });
}
bool DesktopServices::foregroundFullscreen() {
    return windowFullscreen(reinterpret_cast<quintptr>(GetForegroundWindow()));
}
bool DesktopServices::windowFullscreen(quintptr handle) {
    const auto hwnd = reinterpret_cast<HWND>(handle);
    if (!hwnd || hwnd == GetShellWindow() || hwnd == GetDesktopWindow())
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return false;
    std::array<wchar_t, 64> cls{};
    GetClassNameW(hwnd, cls.data(), static_cast<int>(cls.size()));
    if (std::wstring_view(cls.data()) == L"WorkerW" || std::wstring_view(cls.data()) == L"Progman")
        return false; // the wallpaper host is never "fullscreen"
    RECT bounds{};
    if (!GetWindowRect(hwnd, &bounds))
        return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL), &monitor))
        return false;
    RECT overlap{};
    if (!IntersectRect(&overlap, &bounds, &monitor.rcMonitor) ||
        !EqualRect(&overlap, &monitor.rcMonitor))
        return false;
    // Covering the monitor is not enough: a maximized window overhangs it too.
    // Real fullscreen windows have dropped their sizing frame.
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return !(style & (WS_DLGFRAME | WS_THICKFRAME)) &&
           !(exStyle & (WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW));
}
void DesktopServices::suppressAltMenu() {
    if (!(GetAsyncKeyState(VK_MENU) & 0x8000))
        return;
    INPUT tap[2]{};
    for (auto& input : tap) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_CONTROL;
        input.ki.dwExtraInfo = ownInputTag;
    }
    tap[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, tap, sizeof(INPUT));
}
bool DesktopServices::revealInExplorer(const QString& path, const QString& base) {
    const QFileInfo info(base.isEmpty() ? expand(path) : resolvedPath(path, base));
    if (path.isEmpty() || !info.exists())
        return false;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto cleanup = wil::scope_exit([hr] {
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    const auto native = QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(native.c_str(), nullptr, &item, 0, nullptr)) || !item)
        return false;
    // A folder opens itself; a file opens its folder with the file selected.
    const bool shown = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    CoTaskMemFree(item);
    return shown;
}
std::expected<void, QString> DesktopServices::sendPaste(WindowTarget target) {
    if (!isForeground(target))
        return std::unexpected(tx(u"目标窗口已改变，回贴已取消"));
    if ((GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) |
         GetAsyncKeyState(VK_RWIN)) &
        0x8000)
        return std::unexpected(tx(u"请松开修饰键后再回贴"));
    const bool held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    std::vector<INPUT> inputs;
    auto key = [&](WORD vk, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        input.ki.dwExtraInfo = ownInputTag;
        inputs.push_back(input);
    };
    if (!held)
        key(VK_CONTROL, false);
    key('V', false);
    key('V', true);
    if (!held)
        key(VK_CONTROL, true);
    const auto count = static_cast<UINT>(inputs.size());
    if (SendInput(count, inputs.data(), sizeof(INPUT)) != count) {
        INPUT release[2]{};
        release[0].type = INPUT_KEYBOARD;
        release[0].ki.wVk = 'V';
        release[0].ki.dwFlags = KEYEVENTF_KEYUP;
        release[0].ki.dwExtraInfo = ownInputTag;
        release[1] = release[0];
        release[1].ki.wVk = VK_CONTROL;
        SendInput(held ? 1u : 2u, release, sizeof(INPUT));
        return std::unexpected(tx(u"系统拒绝回贴，目标可能以管理员权限运行"));
    }
    return {};
}
std::expected<void, QString> DesktopServices::launch(const LaunchItem& item, const QString& base) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto cleanup = wil::scope_exit([hr] {
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    const auto target = resolvedPath(item.target, base).toStdWString();
    const auto args = item.arguments.toStdWString();
    const auto cwd = item.workingDirectory.isEmpty()
                         ? std::wstring{}
                         : resolvedPath(item.workingDirectory, base).toStdWString();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = item.elevated ? L"runas" : L"open";
    info.lpFile = target.c_str();
    info.lpParameters = args.c_str();
    info.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        return std::unexpected(tx(u"启动失败（系统错误 %1）").arg(GetLastError()));
    return {};
}
std::expected<LaunchItem, QString> DesktopServices::importTarget(const QString& path,
                                                                 const QString& root) {
    LaunchItem item;
    item.id = LauncherCatalog::newId();
    item.target = path;
    item.name = QFileInfo(path).completeBaseName();
    if (item.name.isEmpty())
        item.name = path;
    if (QFileInfo(path).suffix().compare(QStringLiteral("lnk"), Qt::CaseInsensitive) != 0)
        return item;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto cleanup = wil::scope_exit([hr] {
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    wil::com_ptr<IShellLinkW> link;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&link)))) {
        wil::com_ptr<IPersistFile> persist;
        link->QueryInterface(IID_PPV_ARGS(&persist));
        if (persist && SUCCEEDED(persist->Load(path.toStdWString().c_str(), STGM_READ))) {
            std::array<wchar_t, 32768> target{}, args{}, cwd{}, icon{};
            WIN32_FIND_DATAW data{};
            int index = 0;
            link->GetPath(target.data(), static_cast<int>(target.size()), &data, SLGP_RAWPATH);
            link->GetArguments(args.data(), static_cast<int>(args.size()));
            link->GetWorkingDirectory(cwd.data(), static_cast<int>(cwd.size()));
            link->GetIconLocation(icon.data(), static_cast<int>(icon.size()), &index);
            if (target[0]) {
                item.target = QString::fromWCharArray(target.data());
                item.arguments = QString::fromWCharArray(args.data());
                item.workingDirectory = QString::fromWCharArray(cwd.data());
                item.iconPath = QString::fromWCharArray(icon.data());
                return item;
            }
        }
    }
    const auto dir = QDir(root).filePath(QStringLiteral("shortcuts"));
    if (!QDir().mkpath(dir))
        return std::unexpected(tx(u"无法保存特殊快捷方式"));
    const auto copy = QDir(dir).filePath(item.id + QStringLiteral(".lnk"));
    if (!QFile::copy(path, copy))
        return std::unexpected(tx(u"特殊快捷方式复制失败"));
    item.target = copy;
    return item;
}
QImage DesktopServices::fileIcon(const QString& path) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto cleanup = wil::scope_exit([hr] {
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    SHFILEINFOW info{};
    const auto expanded = QDir::toNativeSeparators(expand(path)).toStdWString();
    if (!SHGetFileInfoW(expanded.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
        return {};
    wil::unique_hicon icon(info.hIcon);
    return QImage::fromHICON(icon.get());
}
namespace {
int respondToShellRequest(const QByteArray& payload, bool persistentIcons) {
    QJsonObject response{{QStringLiteral("ok"), false}};
    try {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return 3;
        const auto request = document.object();
        const auto operation = request.value(QStringLiteral("operation")).toString();
        if (persistentIcons && operation != QStringLiteral("icon"))
            return 3;
        const auto root = request.value(QStringLiteral("root")).toString();
        if (operation == QStringLiteral("icon")) {
            const auto source =
                resolvedPath(request.value(QStringLiteral("source")).toString(), root);
            const auto image = DesktopServices::fileIcon(source);
            QByteArray bytes;
            QBuffer output(&bytes);
            output.open(QIODevice::WriteOnly);
            if (!image.isNull() && image.save(&output, "PNG")) {
                response[QStringLiteral("ok")] = true;
                response[QStringLiteral("png")] = QString::fromLatin1(bytes.toBase64());
            } else
                response[QStringLiteral("error")] = tx(u"没有可用图标");
        } else if (operation == QStringLiteral("reveal")) {
            const bool shown = DesktopServices::revealInExplorer(request.value(QStringLiteral("target")).toString(), root);
            response[QStringLiteral("ok")] = shown;
            if (!shown) response[QStringLiteral("error")] = tx(u"文件已不存在或无法定位");
        } else if (operation == QStringLiteral("launch")) {
            const auto o = request.value(QStringLiteral("item")).toObject();
            LaunchItem item;
            item.target = o.value(QStringLiteral("target")).toString();
            item.arguments = o.value(QStringLiteral("arguments")).toString();
            item.workingDirectory = o.value(QStringLiteral("workingDirectory")).toString();
            item.elevated = o.value(QStringLiteral("elevated")).toBool();
            if (item.target.isEmpty())
                return 3;
            const auto result = DesktopServices::launch(item, root);
            response[QStringLiteral("ok")] = result.has_value();
            if (!result)
                response[QStringLiteral("error")] = result.error();
        } else if (operation == QStringLiteral("import")) {
            const auto paths = request.value(QStringLiteral("paths")).toArray();
            if (paths.size() > 1000)
                return 3;
            QJsonArray items;
            bool ok = true;
            for (const auto& value : paths) {
                auto result = DesktopServices::importTarget(value.toString(), root);
                if (!result) {
                    ok = false;
                    response[QStringLiteral("error")] = result.error();
                    break;
                }
                const auto& i = *result;
                items.append(QJsonObject{{QStringLiteral("id"), i.id},
                                         {QStringLiteral("name"), i.name},
                                         {QStringLiteral("target"), i.target},
                                         {QStringLiteral("arguments"), i.arguments},
                                         {QStringLiteral("workingDirectory"), i.workingDirectory},
                                         {QStringLiteral("iconPath"), i.iconPath},
                                         {QStringLiteral("elevated"), i.elevated}});
            }
            response[QStringLiteral("ok")] = ok;
            if (ok)
                response[QStringLiteral("items")] = items;
        } else
            return 3;
    } catch (...) {
        response[QStringLiteral("error")] = tx(u"Shell 处理失败");
    }
    const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) +
                       (persistentIcons ? QByteArrayLiteral("\n") : QByteArray{});
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.constData() + offset,
                       static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
            !written)
            return 3;
        offset += written;
    }
    return 0;
}
} // namespace
int DesktopServices::runShellHelper(bool persistentIcons) {
    QByteArray payload;
    std::array<char, 16384> buffer{};
    DWORD read = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(),
                   static_cast<DWORD>(buffer.size()), &read, nullptr) && read) {
        payload.append(buffer.data(), static_cast<qsizetype>(read));
        if (payload.size() > 8 * 1024 * 1024)
            return 3;
        if (persistentIcons) {
            qsizetype end = 0;
            while ((end = payload.indexOf('\n')) >= 0) {
                const auto request = payload.left(end);
                payload.remove(0, end + 1);
                if (const auto result = respondToShellRequest(request, true); result != 0)
                    return result;
            }
        }
    }
    return persistentIcons ? (payload.isEmpty() ? 0 : 3) : respondToShellRequest(payload, false);
}
std::expected<void, QString> DesktopServices::capturePanelNavigation(bool enabled, bool keepMouse) {
    const bool mouse = enabled || keepMouse;
    if (!enabled) {
        impl_->navigationHook.reset();
        impl_->capturedKeys.fill(false);
    }
    if (!mouse) {
        impl_->mouseHook.reset();
        impl_->sideCapture = false;
        impl_->sideHeld = 0;
    }
    if (!enabled && !mouse) {
        if (Impl::hookOwner == impl_.get())
            Impl::hookOwner = nullptr;
        return {};
    }
    Impl::hookOwner = impl_.get();
    const auto module = GetModuleHandleW(nullptr);
    if (enabled && !impl_->navigationHook)
        impl_->navigationHook.reset(SetWindowsHookExW(WH_KEYBOARD_LL, &Impl::keyboard, module, 0));
    if (mouse && !impl_->mouseHook)
        impl_->mouseHook.reset(SetWindowsHookExW(WH_MOUSE_LL, &Impl::mouse, module, 0));
    if ((enabled && !impl_->navigationHook) || (mouse && !impl_->mouseHook)) {
        const auto error = GetLastError();
        (void)capturePanelNavigation(false);
        return std::unexpected(tx(u"无法启用面板键盘导航（%1）").arg(error));
    }
    return {};
}
std::pair<bool, bool> DesktopServices::panelInputCaptured() const {
    return {static_cast<bool>(impl_->navigationHook), static_cast<bool>(impl_->mouseHook)};
}
void DesktopServices::setSideButtonCapture(bool enabled) {
    impl_->sideCapture = enabled && impl_->mouseHook;
}
} // namespace mt
