#include "LockSession.h"
#include "infrastructure/i18n/I18n.h"
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <wil/resource.h>
#include <windows.h>
#include <wtsapi32.h>

namespace mt {
namespace {
constexpr UINT inputMessage = WM_APP + 41;
constexpr UINT resetMessage = WM_APP + 42;
int qtKey(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return int(vk);
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key_F1 + int(vk - VK_F1);
    switch (vk) {
    case VK_RETURN:
        return Qt::Key_Return;
    case VK_ESCAPE:
        return Qt::Key_Escape;
    case VK_BACK:
        return Qt::Key_Backspace;
    case VK_SPACE:
        return Qt::Key_Space;
    case VK_TAB:
        return Qt::Key_Tab;
    case VK_UP:
        return Qt::Key_Up;
    case VK_DOWN:
        return Qt::Key_Down;
    case VK_LEFT:
        return Qt::Key_Left;
    case VK_RIGHT:
        return Qt::Key_Right;
    case VK_HOME:
        return Qt::Key_Home;
    case VK_END:
        return Qt::Key_End;
    case VK_DELETE:
        return Qt::Key_Delete;
    case VK_INSERT:
        return Qt::Key_Insert;
    default:
        return 0;
    }
}
bool taskManagerForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t path[32768]{};
    DWORD count = DWORD(std::size(path));
    if (!process || !QueryFullProcessImageNameW(process.get(), 0, path, &count))
        return false;
    const auto* slash = wcsrchr(path, L'\\');
    return _wcsicmp(slash ? slash + 1 : path, L"Taskmgr.exe") == 0;
}
bool onInputDesktop() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input)
        return false;
    const auto close = qScopeGuard([&] { CloseDesktop(input); });
    wchar_t inputName[256]{}, threadName[256]{};
    return GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), nullptr) &&
           GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, threadName,
                                     sizeof(threadName), nullptr) &&
           wcscmp(inputName, threadName) == 0;
}
} // namespace
struct LockSession::Impl {
    std::atomic<ULONGLONG> heartbeat{0};
    QMutex windowsMutex;
    QList<quintptr> windows;
    std::array<bool, 256> held{};
    bool caps = false;
    bool capture = false;
    bool queueFailed = false;
    unsigned pending = 0;
    DWORD threadId = 0;
    // Windows invokes WH_KEYBOARD_LL on the installing thread.
    static inline thread_local Impl* current = nullptr;
    void seedKeys() {
        held.fill(false);
        for (int vk = 8; vk < 256; ++vk)
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU)
                held[size_t(vk)] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    }
    void hideCovers() {
        const QMutexLocker guard(&windowsMutex);
        for (const auto handle : windows)
            ShowWindowAsync(reinterpret_cast<HWND>(handle), SW_HIDE);
    }
    static LRESULT CALLBACK hook(int code, WPARAM message, LPARAM data) noexcept {
        auto* self = current;
        if (code != HC_ACTION || !self || !self->capture)
            return CallNextHookEx(nullptr, code, message, data);
        const auto& e = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (e.vkCode >= self->held.size())
            return 1;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool repeat = self->held[e.vkCode];
        self->held[e.vkCode] = down;
        // Debounce each key independently. Waiting for *every* virtual key to
        // become up lets an unrelated stale key disable all unlocking forever.
        // Seeded keys (including Enter/the activation key) must be released
        // before their next down can count; another fresh key remains usable.
        if (!down || repeat)
            return 1;
        if (e.vkCode == VK_CAPITAL)
            self->caps = !self->caps;
        const auto pressed = [self](int a, int b, int c) {
            return self->held[size_t(a)] || self->held[size_t(b)] || self->held[size_t(c)];
        };
        const UINT flags = (pressed(VK_SHIFT, VK_LSHIFT, VK_RSHIFT) ? 1u : 0u) |
                           (pressed(VK_CONTROL, VK_LCONTROL, VK_RCONTROL) ? 2u : 0u) |
                           (pressed(VK_MENU, VK_LMENU, VK_RMENU) ? 4u : 0u) |
                           ((self->held[VK_LWIN] || self->held[VK_RWIN]) ? 8u : 0u) |
                           (self->caps ? 16u : 0u);
        if (self->pending >= 128 ||
            !PostThreadMessageW(self->threadId, inputMessage, e.vkCode,
                                LPARAM((e.scanCode & 0xffffu) | (flags << 16)))) {
            self->queueFailed = true;
            self->capture = false;
            return CallNextHookEx(nullptr, code, message, data);
        }
        ++self->pending;
        return 1;
    }
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        if (msg == WM_POWERBROADCAST || msg == WM_WTSSESSION_CHANGE)
            PostThreadMessageW(GetCurrentThreadId(), resetMessage, 0, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
LockSession::LockSession(QObject* parent) : QThread(parent), impl_(std::make_unique<Impl>()) {}
LockSession::~LockSession() {
    stop();
    // This worker never waits on the GUI or performs filesystem/Shell operations.
    wait();
}
void LockSession::pulse() {
    impl_->heartbeat.store(GetTickCount64());
}
void LockSession::setCoverWindows(const QList<quintptr>& windows) {
    const QMutexLocker guard(&impl_->windowsMutex);
    impl_->windows = windows;
}
void LockSession::stop() {
    requestInterruption();
}
void LockSession::run() {
    auto& state = *impl_;
    state.threadId = GetCurrentThreadId();
    Impl::current = &state;
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // create the thread queue
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Impl::windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"mt.Blackout.Session";
    RegisterClassW(&wc);
    wil::unique_hwnd notification(CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr,
                                                  nullptr, instance, nullptr));
    const bool sessionNotifications =
        notification && WTSRegisterSessionNotification(notification.get(), NOTIFY_FOR_THIS_SESSION);
    state.seedKeys();
    wil::unique_hhook hook(SetWindowsHookExW(WH_KEYBOARD_LL, Impl::hook, instance, 0));
    const auto cleanup = qScopeGuard([&] {
        state.capture = false;
        SetThreadExecutionState(ES_CONTINUOUS);
        if (sessionNotifications)
            WTSUnRegisterSessionNotification(notification.get());
        Impl::current = nullptr;
    });
    const auto power =
        hook ? SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
             : 0;
    QString failure;
    bool emergency = false;
    const bool acquired = notification && sessionNotifications && hook && power;
    if (!acquired)
        failure = tx(u"锁屏启动失败，输入或电源保护未就绪");
    else {
        state.capture = true;
        Q_EMIT ready();
        auto renewed = GetTickCount64();
        bool inputDesktop = true;
        while (!isInterruptionRequested()) {
            const auto now = GetTickCount64();
            if (now - state.heartbeat.load() > 4000 || state.queueFailed) {
                failure = tx(u"锁屏输入响应异常，已解除遮挡");
                break;
            }
            if (taskManagerForeground()) {
                emergency = true;
                break;
            }
            const bool currentDesktop = onInputDesktop();
            if (currentDesktop != inputDesktop) {
                inputDesktop = currentDesktop;
                state.capture = inputDesktop;
                // Ctrl+Alt+Del can move key-up events to the secure desktop.
                // Clear stale modifiers and partial passwords when returning.
                while (PeekMessageW(&message, nullptr, inputMessage, inputMessage, PM_REMOVE)) {
                }
                state.pending = 0;
                state.seedKeys();
                renewed = 0;
                Q_EMIT inputReset();
            }
            // A non-null HHOOK does not prove Windows still has the hook. Renew it
            // without a gap: install the replacement first, then retire the old one.
            if (inputDesktop && now - renewed >= 5000) {
                wil::unique_hhook replacement(
                    SetWindowsHookExW(WH_KEYBOARD_LL, Impl::hook, instance, 0));
                if (!replacement) {
                    failure = tx(u"锁屏键盘保护无法恢复，已解除遮挡");
                    break;
                }
                if (!UnhookWindowsHookEx(hook.get()) &&
                    GetLastError() != ERROR_INVALID_HOOK_HANDLE) {
                    failure = tx(u"锁屏键盘保护无法恢复，已解除遮挡");
                    break;
                }
                (void)hook.release(); // uninstalled successfully, or already removed by Windows
                hook = std::move(replacement);
                renewed = now;
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            // Bound each batch so a busy queue cannot starve stop/lease checks.
            for (int batch = 0; batch < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
                 ++batch) {
                if (message.message == inputMessage) {
                    if (state.pending)
                        --state.pending;
                    const auto vk = UINT(message.wParam);
                    const auto flags = UINT(message.lParam) >> 16;
                    Qt::KeyboardModifiers mods;
                    BYTE keys[256]{};
                    if (flags & 1) {
                        mods |= Qt::ShiftModifier;
                        keys[VK_SHIFT] = 0x80;
                    }
                    if (flags & 2) {
                        mods |= Qt::ControlModifier;
                        keys[VK_CONTROL] = 0x80;
                    }
                    if (flags & 4) {
                        mods |= Qt::AltModifier;
                        keys[VK_MENU] = 0x80;
                    }
                    if (flags & 8)
                        mods |= Qt::MetaModifier;
                    keys[VK_CAPITAL] = (flags & 16) ? 1 : 0;
                    keys[vk] = 0x80;
                    WCHAR chars[8]{};
                    const auto foregroundThread =
                        GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
                    const auto length = ToUnicodeEx(vk, UINT(message.lParam) & 0xffffu, keys, chars,
                                                    8, 4, GetKeyboardLayout(foregroundThread));
                    Q_EMIT keyPressed(qtKey(vk), int(mods), length == 1 ? ushort(chars[0]) : 0);
                } else if (message.message == resetMessage) {
                    // Session/desktop transitions can lose key-up messages. Retake
                    // the initial key state and renew immediately on return.
                    state.seedKeys();
                    renewed = 0;
                    Q_EMIT inputReset();
                } else {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
    }
    // Even if unhooking fails, the callback is inert until this thread exits.
    state.capture = false;
    if (hook) {
        if (!UnhookWindowsHookEx(hook.get()) && GetLastError() != ERROR_INVALID_HOOK_HANDLE)
            failure = tx(u"锁屏键盘钩子释放失败，输入线程已停止");
        else
            (void)hook.release();
    }
    // The activating chord's release may have been swallowed after its press
    // reached the old foreground window. Balance modifiers before returning it.
    const WORD modifiers[]{VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                           VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN};
    std::array<INPUT, std::size(modifiers)> releases{};
    for (size_t i = 0; i < releases.size(); ++i) {
        releases[i].type = INPUT_KEYBOARD;
        releases[i].ki.wVk = modifiers[i];
        releases[i].ki.dwFlags = KEYEVENTF_KEYUP;
        releases[i].ki.dwExtraInfo = 0x544F4F4C;
    }
    if (acquired)
        SendInput(UINT(releases.size()), releases.data(), sizeof(INPUT));
    SetThreadExecutionState(ES_CONTINUOUS);
    if (emergency || !failure.isEmpty())
        state.hideCovers();
    Impl::current = nullptr;
    if (emergency)
        Q_EMIT emergencyExit();
    if (!failure.isEmpty())
        Q_EMIT failed(failure);
}
} // namespace mt
