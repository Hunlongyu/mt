#include <windows.h>
#include "GestureInput.h"
#include "InputTag.h"
#include "modules/gestures/GestureCapture.h"
#include "infrastructure/i18n/I18n.h"
#include <QGuiApplication>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QtGui/qscreen_platform.h>
#include <wil/resource.h>
#include <array>
#include <deque>
#include <mutex>

namespace mt {
namespace {
HWND hwnd(WindowTarget target) { return reinterpret_cast<HWND>(target.handle); }
// Higher-integrity presses require a connected helper for both actions and
// ordinary click replay; unknown/system integrity is never admitted.
DWORD integrity(DWORD pid) {
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE raw = nullptr;
    if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) return MAXDWORD;
    wil::unique_handle token(raw);
    alignas(TOKEN_MANDATORY_LABEL)
        std::array<std::byte, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = DWORD(buffer.size());
    if (!GetTokenInformation(token.get(), TokenIntegrityLevel, buffer.data(), size, &size))
        return MAXDWORD;
    const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
    return *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
}
struct Button {
    WPARAM down = WM_RBUTTONDOWN, up = WM_RBUTTONUP;
    int vk = VK_RBUTTON;
    DWORD downFlag = MOUSEEVENTF_RIGHTDOWN, upFlag = MOUSEEVENTF_RIGHTUP;
    WORD x = 0;
};
Button buttonFor(const QString& key) {
    if (key == QStringLiteral("middle"))
        return {WM_MBUTTONDOWN, WM_MBUTTONUP, VK_MBUTTON, MOUSEEVENTF_MIDDLEDOWN,
                MOUSEEVENTF_MIDDLEUP, 0};
    if (key == QStringLiteral("x1") || key == QStringLiteral("x2")) {
        const bool first = key == QStringLiteral("x1");
        return {WM_XBUTTONDOWN, WM_XBUTTONUP, first ? VK_XBUTTON1 : VK_XBUTTON2,
                MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, WORD(first ? XBUTTON1 : XBUTTON2)};
    }
    return {};
}
bool allMouseReleased() {
    return !((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
              GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) |
              GetAsyncKeyState(VK_XBUTTON2)) & 0x8000);
}
HWND ownerAt(POINT point) {
    const auto hit = WindowFromPoint(point);
    DWORD pid = 0;
    GetWindowThreadProcessId(hit, &pid);
    // Owned menus/dialogs must not inherit the launcher's opt-in via ROOTOWNER.
    if (pid == GetCurrentProcessId()) return GetAncestor(hit, GA_ROOT);
    const auto owner = GetAncestor(hit, GA_ROOTOWNER);
    return owner ? owner : hit;
}
} // namespace
struct GestureInput::Impl final : QObject {
    GestureInput* owner;
    QThread thread;
    QTimer* timer = nullptr;
    wil::unique_hhook mouseHook, keyboardHook;
    GestureInputOptions options;
    GestureCapture capture;
    struct FinishedStroke {
        GestureCapture capture;
        Button button;
        WindowTarget target;
        HWND foreground = nullptr;
        QString process;
        quint64 generation = 0, cancellation = 0, stroke = 0;
        bool admin = false;
    };
    static constexpr size_t completionLimit = 4;
    std::array<FinishedStroke, completionLimit> finishedStrokes;
    size_t finishedHead = 0, finishedCount = 0;
    Button button, capturedButton;
    WindowTarget candidate, target;
    HWND foregroundAtStart = nullptr;
    QString candidateProcess, targetProcess;
    DWORD ownIntegrity = MAXDWORD;
    bool permitted = false, hookErrorReported = false;
    bool candidateAdmin = false, targetAdmin = false;
    quint64 generation = 0, captureGeneration = 0, captureCancellation = 0, stroke = 0, published = 0;
    quint64 begun = 0, renewed = 0, gated = 0;
    std::mutex deliveryMutex;
    std::optional<GestureFrame> latestFrame;
    std::deque<GestureFrame> completedFrames;
    bool deliveryQueued = false;
    static inline thread_local Impl* current = nullptr;

    explicit Impl(GestureInput* input) : owner(input) {
        moveToThread(&thread);
        thread.setObjectName(QStringLiteral("mt.gesture-input"));
        thread.start();
    }
    void report(QString message) {
        QMetaObject::invokeMethod(owner, [o = owner, message = std::move(message)] {
            Q_EMIT o->failed(message);
        }, Qt::QueuedConnection);
    }
    void publish(GestureFrame frame) {
        {
            const std::lock_guard lock(deliveryMutex);
            if (frame.finished) {
                if (latestFrame && latestFrame->stroke <= frame.stroke) latestFrame.reset();
                if (completedFrames.size() == completionLimit) completedFrames.pop_front();
                completedFrames.push_back(std::move(frame));
            } else {
                latestFrame = std::move(frame);
            }
            if (deliveryQueued) return;
            deliveryQueued = true;
        }
        // Coalesce live drawing, but retain bounded terminal frames: a fast new
        // press must not overwrite the preceding release before the GUI reads it.
        QMetaObject::invokeMethod(owner, [this, o = owner] {
            std::optional<GestureFrame> frame;
            std::deque<GestureFrame> completed;
            {
                const std::lock_guard lock(deliveryMutex);
                completed.swap(completedFrames);
                frame = std::move(latestFrame);
                latestFrame.reset();
                deliveryQueued = false;
            }
            for (const auto& terminal : completed) Q_EMIT o->frame(terminal);
            if (frame) Q_EMIT o->frame(*frame);
        }, Qt::QueuedConnection);
    }
    bool install() {
        const auto module = GetModuleHandleW(nullptr);
        wil::unique_hhook nextMouse(SetWindowsHookExW(WH_MOUSE_LL, &Impl::mouse, module, 0));
        wil::unique_hhook nextKeyboard(SetWindowsHookExW(WH_KEYBOARD_LL, &Impl::keyboard, module, 0));
        if (!nextMouse || !nextKeyboard) return false;
        mouseHook = std::move(nextMouse);
        keyboardHook = std::move(nextKeyboard);
        current = this;
        renewed = GetTickCount64();
        return true;
    }
    std::expected<void, QString> configure(const GestureInputOptions& next, quint64 epoch) {
        if (next.enabled && !mouseHook && !install())
            return std::unexpected(tx(u"无法安装鼠标手势钩子"));
        if (!timer) {
            ownIntegrity = integrity(GetCurrentProcessId());
            timer = new QTimer(this);
            timer->setInterval(8);
            timer->setTimerType(Qt::PreciseTimer);
            connect(timer, &QTimer::timeout, this, [this] { pump(); });
        }
        capture.cancel(true);
        options = next;
        button = buttonFor(next.trigger);
        generation = epoch;
        permitted = false;
        gated = 0;
        timer->start();
        return {};
    }
    void gate(POINT point) {
        permitted = false;
        candidate = DesktopServices::windowTarget(reinterpret_cast<quintptr>(ownerAt(point)));
        candidate.focusedControl = 0; // the gesture owns a window, not a caret
        if (!candidate.handle || !candidate.processStarted ||
            (candidate.processId == GetCurrentProcessId() &&
             candidate.handle != owner->launcherWindow_.load())) return;
        candidateProcess = DesktopServices::processName(candidate);
        const auto level = integrity(candidate.processId);
        candidateAdmin = level > ownIntegrity;
        permitted = ownIntegrity != MAXDWORD && level != MAXDWORD &&
                    level <= SECURITY_MANDATORY_HIGH_RID &&
                    (!candidateAdmin || owner->adminAvailable_.load()) &&
                    options.rules.permits(candidateProcess);
    }
    bool admit(POINT point, quint64 now) {
        const auto window = ownerAt(point);
        DWORD pid = 0;
        if (!window || !GetWindowThreadProcessId(window, &pid)) return false;
        const bool ownWindow = pid == GetCurrentProcessId();
        if (ownWindow && reinterpret_cast<quintptr>(window) != owner->launcherWindow_.load()) return false;
        // The idle cache is an optimization, never a prerequisite for owning a
        // press. A cache miss uses native identity/token queries at this point;
        // no Shell, file I/O or nested event loop.
        if (ownWindow || window != hwnd(candidate) || pid != candidate.processId || now - gated >= 100) {
            gate(point);
            gated = now;
        }
        return permitted && (!candidateAdmin || owner->adminAvailable_.load()) &&
               !(options.suppressFullscreen && DesktopServices::windowFullscreen(candidate.handle));
    }
    void finishCapture() {
        // Fixed storage in the callback; even an overrun keeps owning every
        // Down/Up and drops an old result instead of leaking a new native drag.
        if (finishedCount == completionLimit) {
            finishedHead = (finishedHead + 1) % completionLimit;
            --finishedCount;
        }
        auto& finished = finishedStrokes[(finishedHead + finishedCount) % completionLimit];
        finished.capture = capture;
        finished.button = capturedButton;
        finished.target = target;
        finished.foreground = foregroundAtStart;
        finished.process = targetProcess;
        finished.generation = captureGeneration;
        finished.cancellation = captureCancellation;
        finished.stroke = stroke;
        finished.admin = targetAdmin;
        ++finishedCount;
        capture.reset();
    }
    GestureFrame snapshot(const GestureCapture& state, WindowTarget window, HWND foreground,
                          const QString& process, quint64 epoch, quint64 cancellation,
                          quint64 id, bool finished) const {
        GestureFrame frame;
        frame.generation = epoch;
        frame.cancellation = cancellation;
        frame.stroke = id;
        frame.target = window;
        frame.foregroundAtStart = reinterpret_cast<quintptr>(foreground);
        frame.process = process;
        frame.drawing = state.drawing();
        frame.finished = finished;
        frame.cancelled = state.cancelled() || state.result() == GestureCapture::Result::Click;
        frame.points.reserve(qsizetype(state.points().size()));
        for (const auto point : state.points()) frame.points.append(point);
        return frame;
    }
    bool replay(const Button& replayButton, bool release = true) {
        INPUT input[2]{};
        for (auto& e : input) {
            e.type = INPUT_MOUSE;
            e.mi.mouseData = replayButton.x;
            e.mi.dwExtraInfo = ownInputTag;
        }
        input[0].mi.dwFlags = replayButton.downFlag;
        input[1].mi.dwFlags = replayButton.upFlag;
        const UINT count = release ? 2 : 1;
        const auto sent = SendInput(count, input, sizeof(INPUT));
        if (sent == 1 && release) SendInput(1, &input[1], sizeof(INPUT));
        return sent == count;
    }
    void pump() {
        const auto now = GetTickCount64();
        if (capture.held()) {
            if (owner->blocked_.load() || owner->cancellation() != captureCancellation ||
                (targetAdmin && !owner->adminAvailable_.load()) ||
                GetForegroundWindow() != foregroundAtStart)
                capture.cancel();
            else if (!options.enabled)
                capture.cancel(true);
            capture.expire(now, options.timeout);
            // A swallowed Down does not reliably update GetAsyncKeyState.
            // Keep ownership until the observed Up; never infer release from it.
        }
        while (finishedCount) {
            const auto finished = std::move(finishedStrokes[finishedHead]);
            finishedHead = (finishedHead + 1) % completionLimit;
            --finishedCount;
            if (finished.capture.result() == GestureCapture::Result::Click) {
                POINT pointer{};
                GetCursorPos(&pointer);
                // Never replay a delayed click onto an unrelated window or location.
                const auto delta = QPoint(pointer.x, pointer.y) - finished.capture.points().back();
                if (DesktopServices::validTarget(finished.target) &&
                    GetForegroundWindow() == finished.foreground &&
                    ownerAt(pointer) == hwnd(finished.target) && delta.manhattanLength() <= 8) {
                    if (finished.admin) {
                        const auto key = finished.button.x == XBUTTON1   ? QStringLiteral("x1")
                                         : finished.button.x == XBUTTON2 ? QStringLiteral("x2")
                                         : finished.button.vk == VK_MBUTTON
                                             ? QStringLiteral("middle")
                                             : QStringLiteral("right");
                        QMetaObject::invokeMethod(
                            owner,
                            [o = owner, finished, key, pointer] {
                                Q_EMIT o->clickReplayRequested(
                                    finished.target, quintptr(finished.foreground),
                                    QPoint(pointer.x, pointer.y), key, finished.generation,
                                    finished.cancellation);
                            },
                            Qt::QueuedConnection);
                    } else if (!replay(finished.button))
                        report(tx(u"系统拒绝恢复鼠标点击"));
                }
            }
            publish(snapshot(finished.capture, finished.target, finished.foreground, finished.process,
                             finished.generation, finished.cancellation, finished.stroke, true));
        }
        if (capture.held() && published != capture.revision()) {
            publish(snapshot(capture, target, foregroundAtStart, targetProcess,
                             captureGeneration, captureCancellation, stroke, false));
            published = capture.revision();
        }
        if (!capture.held()) {
            if (!options.enabled) {
                mouseHook.reset();
                keyboardHook.reset();
                timer->stop();
            } else {
                POINT pointer{};
                const bool movedToAnotherWindow = GetCursorPos(&pointer) && ownerAt(pointer) != hwnd(candidate);
                if (movedToAnotherWindow || now - gated >= 100) {
                    if (GetCursorPos(&pointer)) gate(pointer);
                    gated = now;
                }
                // Windows can silently remove a low-level hook. Renew while idle.
                if (now - renewed >= 5000 && allMouseReleased()) {
                    if (!install() && !hookErrorReported) {
                        hookErrorReported = true;
                        report(tx(u"鼠标手势钩子恢复失败，请重新启用手势"));
                    }
                }
            }
        }
        // A removed hook may have lost Up. Retain the pairing state across renewal
        // so a late Up is consumed instead of reaching an unrelated application.
        if (capture.held() && capture.cancelled() && now - renewed >= 5000) {
            if (!install() && !hookErrorReported) {
                hookErrorReported = true;
                report(tx(u"鼠标手势钩子恢复失败，请重新启用手势"));
            }
        }
    }
    static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) noexcept {
        auto* self = current;
        if (code != HC_ACTION || !self) return CallNextHookEx(nullptr, code, message, data);
        const auto& e = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
        if (e.dwExtraInfo == ownInputTag) return CallNextHookEx(nullptr, code, message, data);
        try {
            const auto now = GetTickCount64();
            const QPoint point(e.pt.x, e.pt.y);
            const auto& b = self->capture.held() ? self->capturedButton : self->button;
            const bool matching = !b.x || HIWORD(e.mouseData) == b.x;
            if (self->capture.held()) {
                if (message == b.down && matching) {
                    // A second Down implies an Up was lost while the hook was
                    // absent. Consume this whole new pair without any action.
                    self->capture.cancel();
                    self->capture.release(point, now);
                    self->capture.begin(point, now);
                    self->capture.cancel();
                    return 1;
                }
                if (self->owner->blocked_.load() || self->owner->cancellation() != self->captureCancellation ||
                    GetForegroundWindow() != self->foregroundAtStart)
                    self->capture.cancel();
                self->capture.expire(now, self->options.timeout);
                if (message == b.up && matching) {
                    self->capture.release(point, now);
                    self->finishCapture();
                    return 1;
                }
                if (message == WM_MOUSEMOVE) self->capture.move(point, now);
                else if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                         message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                         message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
                    self->capture.cancel();
            } else if (self->options.enabled &&
                       !self->owner->blocked_.load() && message == b.down && matching &&
                       DesktopServices::modifiersReleased() &&
                       self->admit(e.pt, now)) {
                self->target = self->candidate;
                self->targetAdmin = self->candidateAdmin;
                self->foregroundAtStart = GetForegroundWindow();
                self->captureGeneration = self->generation;
                self->captureCancellation = self->owner->cancellation();
                self->targetProcess = self->candidateProcess; // implicitly shared, no detach
                self->capturedButton = b;
                self->capture.begin(point, now);
                self->begun = now;
                ++self->stroke;
                self->owner->stroke_.store(self->stroke);
                return 1;
            }
        } catch (...) {
            self->capture.cancel();
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    static LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM data) noexcept {
        auto* self = current;
        if (code == HC_ACTION && self) {
            const auto& e = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
            if (e.dwExtraInfo != ownInputTag && self->capture.held() &&
                (message == WM_KEYDOWN || message == WM_SYSKEYDOWN))
                self->capture.cancel(); // Escape, modifiers or typing invalidate the action
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    void shutdown() {
        if (timer) timer->stop();
        // At process exit there is no callback left to swallow the eventual Up.
        // Hand ownership back with a tagged Down (and Up if already released).
        // A privileged press has not injected any Down. Cancel it on teardown;
        // do not synthesize an unmatched privileged button while the helper exits.
        if (capture.held() && !targetAdmin)
            replay(capturedButton, !(GetAsyncKeyState(capturedButton.vk) & 0x8000));
        mouseHook.reset();
        keyboardHook.reset();
        current = nullptr;
        delete timer;
        timer = nullptr;
        moveToThread(owner->thread());
    }
};
GestureInput::GestureInput(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
GestureInput::~GestureInput() {
    blocked_.store(true);
    QMetaObject::invokeMethod(impl_.get(), [this] { impl_->shutdown(); }, Qt::BlockingQueuedConnection);
    impl_->thread.quit();
    impl_->thread.wait(); // worker never waits on the GUI or performs Shell work
}
std::expected<void, QString> GestureInput::configure(const GestureInputOptions& options) {
    std::expected<void, QString> result;
    const auto next = generation_ + 1;
    QMetaObject::invokeMethod(impl_.get(), [&] { result = impl_->configure(options, next); },
                              Qt::BlockingQueuedConnection);
    if (result) generation_ = next;
    return result;
}
bool GestureInput::isCurrent(const GestureFrame& frame) {
    return DesktopServices::validTarget(frame.target) &&
           reinterpret_cast<quintptr>(GetForegroundWindow()) == frame.foregroundAtStart;
}
QPointF GestureInput::logicalPoint(QPoint physical) {
    const POINT point{physical.x(), physical.y()};
    const auto handle = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(handle, &info)) return physical;
    for (auto* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == handle) {
            const auto scale = screen->devicePixelRatio();
            return QPointF(screen->geometry().topLeft()) + QPointF((physical.x() - info.rcMonitor.left) / scale,
                                                                  (physical.y() - info.rcMonitor.top) / scale);
        }
    }
    return physical;
}
} // namespace mt
