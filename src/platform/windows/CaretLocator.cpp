#include <windows.h>

#include "CaretLocator.h"
#include <QPointer>
#include <imm.h>
#include <oleacc.h>
#include <shellscalingapi.h>
#include <uiautomation.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string_view>

namespace mt {
namespace {
// Registering any focus listener marks this process as an assistive-technology
// client; Chromium and CEF then build the accessibility tree that exposes their
// caret. The handler itself does nothing.
class SilentFocusHandler final : public IUIAutomationFocusChangedEventHandler {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *out = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&references_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&references_);
        if (!left)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement*) override {
        return S_OK;
    }

  private:
    LONG references_ = 1;
};
// Client coordinates of another process's window are in that window's own DPI
// space: a DPI-unaware or system-aware app on a scaled monitor reports them
// unscaled. Convert in the window's context, then to physical pixels.
// A screen point in `window`'s own DPI space (what a DPI-unaware or system-aware
// app reports) mapped to physical pixels through its top-level window: that
// rectangle seen in the app's context and in physical pixels gives scale and
// offset. (The system's LogicalToPhysicalPoint rejects many child windows,
// e.g. the Qt input window inside DingTalk.)
std::optional<POINT> screenToPhysical(HWND window, POINT point) {
    const auto context = GetWindowDpiAwarenessContext(window);
    if (GetAwarenessFromDpiAwarenessContext(context) == DPI_AWARENESS_PER_MONITOR_AWARE)
        return point;
    const HWND root = GetAncestor(window, GA_ROOT);
    RECT logicalRoot{}, physicalRoot{};
    const auto previous = SetThreadDpiAwarenessContext(context);
    const bool seen = GetWindowRect(root, &logicalRoot);
    if (previous)
        SetThreadDpiAwarenessContext(previous);
    if (!seen || !GetWindowRect(root, &physicalRoot) || logicalRoot.right <= logicalRoot.left ||
        logicalRoot.bottom <= logicalRoot.top)
        return std::nullopt;
    const double sx = double(physicalRoot.right - physicalRoot.left) / (logicalRoot.right - logicalRoot.left);
    const double sy = double(physicalRoot.bottom - physicalRoot.top) / (logicalRoot.bottom - logicalRoot.top);
    return POINT{physicalRoot.left + LONG(std::lround((point.x - logicalRoot.left) * sx)),
                 physicalRoot.top + LONG(std::lround((point.y - logicalRoot.top) * sy))};
}
// Client coordinates of another process's window are in that window's own DPI space.
std::optional<POINT> clientToPhysical(HWND window, POINT point) {
    const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
    const bool mapped = ClientToScreen(window, &point);
    if (previous)
        SetThreadDpiAwarenessContext(previous);
    return mapped ? screenToPhysical(window, point) : std::nullopt;
}
std::optional<QRect> screenRectToPhysical(HWND window, LONG x, LONG y, LONG w, LONG h) {
    const auto topLeft = screenToPhysical(window, {x, y});
    const auto bottomRight = screenToPhysical(window, {x + w, y + h});
    if (!topLeft || !bottomRight)
        return std::nullopt;
    return QRect(QPoint(topLeft->x, topLeft->y), QPoint(bottomRight->x - 1, bottomRight->y - 1));
}
std::optional<QRect> clientRectToPhysical(HWND window, const RECT& r) {
    const auto topLeft = clientToPhysical(window, {r.left, r.top});
    const auto bottomRight = clientToPhysical(window, {r.right, r.bottom});
    if (!topLeft || !bottomRight)
        return std::nullopt;
    return QRect(QPoint(topLeft->x, topLeft->y), QPoint(bottomRight->x - 1, bottomRight->y - 1));
}
// Some apps (DingTalk) draw a 1-3 px tall system caret. Its top is the line
// start; give it the height of an ordinary text line on that monitor.
QRect withLineHeight(QRect caret) {
    if (caret.height() >= 4)
        return caret;
    UINT dpiX = 96, dpiY = 96;
    const POINT at{caret.left(), caret.top()};
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI,
                                &dpiX, &dpiY)))
        dpiY = 96;
    caret.setHeight(MulDiv(18, static_cast<int>(dpiY), 96));
    return caret;
}
QRect toRect(const RECT& r) {
    return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
}
std::optional<QRect> firstRect(IUIAutomationTextRange* range) {
    SAFEARRAY* raw = nullptr;
    if (!range || FAILED(range->GetBoundingRectangles(&raw)) || !raw)
        return std::nullopt;
    auto destroy = wil::scope_exit([raw] { SafeArrayDestroy(raw); });
    LONG lower = 0, upper = -1;
    SafeArrayGetLBound(raw, 1, &lower);
    SafeArrayGetUBound(raw, 1, &upper);
    double* values = nullptr;
    if (upper - lower + 1 < 4 || FAILED(SafeArrayAccessData(raw, reinterpret_cast<void**>(&values))))
        return std::nullopt;
    const QRect rect(qRound(values[0]), qRound(values[1]), qMax(1, qRound(values[2])),
                     qRound(values[3]));
    SafeArrayUnaccessData(raw);
    if (rect.height() <= 0)
        return std::nullopt;
    return rect;
}
// A collapsed caret range has no rectangle of its own: use the edge of the
// character before it, after it, or the end of its line on an empty line.
std::optional<QRect> caretOfRange(IUIAutomationTextRange* range) {
    if (const auto own = firstRect(range))
        return own;
    wil::com_ptr<IUIAutomationTextRange> probe;
    int moved = 0;
    if (SUCCEEDED(range->Clone(&probe)) &&
        SUCCEEDED(probe->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1,
                                            &moved)) &&
        moved)
        if (const auto before = firstRect(probe.get()))
            return QRect(before->right() + 1, before->top(), 2, before->height());
    probe.reset();
    if (SUCCEEDED(range->Clone(&probe)) &&
        SUCCEEDED(probe->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1,
                                            &moved)) &&
        moved)
        if (const auto after = firstRect(probe.get()))
            return QRect(after->left(), after->top(), 2, after->height());
    probe.reset();
    if (SUCCEEDED(range->Clone(&probe)) && SUCCEEDED(probe->ExpandToEnclosingUnit(TextUnit_Line)))
        if (const auto line = firstRect(probe.get()))
            return QRect(line->right() + 1, line->top(), 2, line->height());
    return std::nullopt;
}
bool isChromiumWindow(HWND window) {
    std::array<wchar_t, 32> name{};
    return window && GetClassNameW(window, name.data(), static_cast<int>(name.size())) &&
           std::wstring_view(name.data()).starts_with(L"Chrome_WidgetWin");
}
} // namespace
// Joins the foreground thread's input state for the lifetime of the object, so
// GetFocus/GetCaretPos/ImmGetContext answer for that thread. Kept very short.
class AttachedInput {
  public:
    explicit AttachedInput(DWORD thread)
        : self_(GetCurrentThreadId()), other_(thread),
          attached_(thread && thread != self_ && AttachThreadInput(self_, thread, TRUE)) {}
    ~AttachedInput() {
        if (attached_)
            AttachThreadInput(self_, other_, FALSE);
    }
    explicit operator bool() const {
        return attached_;
    }

  private:
    DWORD self_;
    DWORD other_;
    bool attached_;
};
struct CaretLocator::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::function<void()> pending;
    bool stopping = false;
    std::thread worker;
    bool comInitialized = false;
    mutable QString trace;
    wil::com_ptr<IUIAutomation> automation;
    wil::com_ptr<IUIAutomationFocusChangedEventHandler> focusHandler;
    void initialize();
    void shutdown();
    std::optional<QRect> locate(HWND foreground) const;

    std::optional<QRect> win32(const GUITHREADINFO& thread) const {
        if (!thread.hwndCaret)
            return std::nullopt;
        return clientRectToPhysical(thread.hwndCaret, thread.rcCaret);
    }
    // Custom-drawn editors (Weixin, many IM clients) move a hidden system caret
    // that only the owning thread can read.
    std::optional<QRect> attachedCaret(DWORD thread, HWND caretWindow) const {
        const AttachedInput attached(thread);
        // GetCaretPos is relative to the window that owns the caret.
        const HWND owner = caretWindow ? caretWindow : attached ? GetFocus() : nullptr;
        POINT point{};
        if (!attached || !owner || !GetCaretPos(&point) || (point.x == 0 && point.y == 0))
            return std::nullopt;
        // A nominal one-line caret in the owner's own coordinates.
        return clientRectToPhysical(owner, {point.x, point.y, point.x + 2, point.y + 18});
    }
    // Editors tell the input method where to draw its candidate window; that is
    // the caret even when nothing else exposes it (CEF, custom-drawn inputs).
    std::optional<QRect> imeCaret(DWORD thread) const {
        const AttachedInput attached(thread);
        const HWND focus = attached ? GetFocus() : nullptr;
        const HIMC context = focus ? ImmGetContext(focus) : nullptr;
        if (!context)
            return std::nullopt;
        auto release = wil::scope_exit([&] { ImmReleaseContext(focus, context); });
        const int lineHeight = MulDiv(18, static_cast<int>(GetDpiForWindow(focus)), 96);
        std::optional<QRect> local;
        CANDIDATEFORM candidate{};
        COMPOSITIONFORM composition{};
        if (ImmGetCandidateWindow(context, 0, &candidate) && candidate.dwStyle) {
            if ((candidate.dwStyle & CFS_EXCLUDE) && candidate.rcArea.bottom > candidate.rcArea.top)
                local = QRect(QPoint(candidate.rcArea.left, candidate.rcArea.top),
                              QPoint(candidate.rcArea.right - 1, candidate.rcArea.bottom - 1));
            else if (candidate.ptCurrentPos.x || candidate.ptCurrentPos.y)
                // The candidate point is where the list starts: just below the line.
                local = QRect(candidate.ptCurrentPos.x, candidate.ptCurrentPos.y - lineHeight, 2,
                              lineHeight);
        }
        if (!local && ImmGetCompositionWindow(context, &composition) && composition.dwStyle &&
            (composition.ptCurrentPos.x || composition.ptCurrentPos.y))
            local = QRect(composition.ptCurrentPos.x, composition.ptCurrentPos.y, 2, lineHeight);
        if (!local)
            return std::nullopt;
        return clientRectToPhysical(
            focus, {local->left(), local->top(), local->right() + 1, local->bottom() + 1});
    }
    std::optional<QRect> msaa(const GUITHREADINFO& thread) const {
        const HWND focus = thread.hwndFocus ? thread.hwndFocus : thread.hwndActive;
        wil::com_ptr<IAccessible> caret;
        if (!focus || FAILED(AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CARET),
                                                        IID_PPV_ARGS(&caret))))
            return std::nullopt;
        VARIANT self{};
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        LONG x = 0, y = 0, w = 0, h = 0;
        if (FAILED(caret->accLocation(&x, &y, &w, &h, self)) || h <= 0)
            return std::nullopt;
        // MSAA reports screen coordinates in the app's own DPI space.
        return screenRectToPhysical(focus, x, y, qMax(2L, w), h);
    }
    // Last resort when no caret exists at all (CEF removes it from an empty
    // input box): the focused editable element; text starts at its top-left.
    std::optional<QRect> focusedEditable(const GUITHREADINFO& thread) const {
        const HWND focus = thread.hwndFocus;
        wil::com_ptr<IAccessible> client;
        if (!focus || FAILED(AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CLIENT),
                                                        IID_PPV_ARGS(&client))))
            return std::nullopt;
        wil::unique_variant focused;
        if (FAILED(client->get_accFocus(&focused)))
            return std::nullopt;
        wil::com_ptr<IAccessible> element = client;
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = CHILDID_SELF;
        if (focused.vt == VT_DISPATCH && focused.pdispVal) {
            if (FAILED(focused.pdispVal->QueryInterface(IID_PPV_ARGS(&element))))
                return std::nullopt;
        } else if (focused.vt == VT_I4) {
            child.lVal = focused.lVal;
        } else {
            return std::nullopt;
        }
        wil::unique_variant role;
        if (FAILED(element->get_accRole(child, &role)) || role.vt != VT_I4 ||
            role.lVal != ROLE_SYSTEM_TEXT)
            return std::nullopt;
        LONG x = 0, y = 0, w = 0, h = 0;
        if (FAILED(element->accLocation(&x, &y, &w, &h, child)) || w <= 0 || h <= 0)
            return std::nullopt;
        // A nominal caret at the element's text origin; height comes from withLineHeight.
        return screenRectToPhysical(focus, x, y, 2, 1);
    }
    std::optional<QRect> uia() const {
        if (!automation)
            return std::nullopt;
        wil::com_ptr<IUIAutomationCacheRequest> cache;
        if (FAILED(automation->CreateCacheRequest(&cache)) || !cache)
            return std::nullopt;
        cache->AddPattern(UIA_TextPatternId);
        cache->AddPattern(UIA_TextPattern2Id);
        wil::com_ptr<IUIAutomationElement> element;
        wil::com_ptr<IUIAutomationTextPattern> text;
        // A Chromium window whose tree is not built yet first exposes only its
        // outer widget; asking it for its accessible object once builds the tree.
        for (int attempt = 0; attempt < 2 && !text; ++attempt) {
            element.reset();
            if (FAILED(automation->GetFocusedElementBuildCache(cache.get(), &element)) || !element)
                return std::nullopt;
            if (SUCCEEDED(element->GetCachedPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) && text)
                break;
            UIA_HWND native = nullptr;
            element->get_CurrentNativeWindowHandle(&native);
            const auto window = static_cast<HWND>(native);
            if (!isChromiumWindow(window))
                break;
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(window, WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CLIENT),
                                SMTO_ABORTIFHUNG, 50, &ignored);
        }
        if (!text) {
            // WPF publishes the caret as its own element.
            return wpf(element.get());
        }
        wil::com_ptr<IUIAutomationTextPattern2> text2;
        if (SUCCEEDED(element->GetCachedPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&text2))) && text2) {
            BOOL active = FALSE;
            wil::com_ptr<IUIAutomationTextRange> caret;
            if (SUCCEEDED(text2->GetCaretRange(&active, &caret)) && caret)
                if (const auto found = caretOfRange(caret.get()))
                    return found;
        }
        wil::com_ptr<IUIAutomationTextRangeArray> selection;
        int count = 0;
        wil::com_ptr<IUIAutomationTextRange> first;
        if (SUCCEEDED(text->GetSelection(&selection)) && selection &&
            SUCCEEDED(selection->get_Length(&count)) && count > 0 &&
            SUCCEEDED(selection->GetElement(0, &first)) && first)
            if (const auto found = caretOfRange(first.get()))
                return found;
        return wpf(element.get());
    }
    std::optional<QRect> wpf(IUIAutomationElement* element) const {
        if (!element)
            return std::nullopt;
        wil::unique_variant name;
        name.vt = VT_BSTR;
        name.bstrVal = SysAllocString(L"WpfCaret");
        wil::com_ptr<IUIAutomationCondition> condition;
        wil::com_ptr<IUIAutomationElement> caret;
        RECT bounds{};
        if (!name.bstrVal ||
            FAILED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, name, &condition)) ||
            FAILED(element->FindFirst(TreeScope_Descendants, condition.get(), &caret)) || !caret ||
            FAILED(caret->get_CurrentBoundingRectangle(&bounds)))
            return std::nullopt;
        return toRect(bounds);
    }
};
void CaretLocator::Impl::initialize() {
    // Create an input queue for AttachThreadInput, without creating a window.
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (!comInitialized)
        return;
    wil::com_ptr<IUIAutomation> created;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&created))) &&
        FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&created))))
        return;
    // A hung target must never freeze the hotkey: cap every cross-process call.
    if (const auto timed = created.try_query<IUIAutomation2>()) {
        timed->put_ConnectionTimeout(300);
        timed->put_TransactionTimeout(300);
    }
    automation = created;
    focusHandler.attach(new SilentFocusHandler);
    if (FAILED(automation->AddFocusChangedEventHandler(nullptr, focusHandler.get())))
        focusHandler.reset();
}
void CaretLocator::Impl::shutdown() {
    if (automation && focusHandler)
        automation->RemoveFocusChangedEventHandler(focusHandler.get());
    focusHandler.reset();
    automation.reset();
    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
}
CaretLocator::CaretLocator() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([worker = impl_.get()] {
        worker->initialize();
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(worker->mutex);
                worker->wake.wait(lock, [worker] { return worker->stopping || worker->pending; });
                if (worker->stopping)
                    break;
                task = std::move(worker->pending);
                worker->pending = {};
            }
            task();
        }
        worker->shutdown();
    });
}
CaretLocator::~CaretLocator() {
    ++generation_;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->pending = {};
    }
    impl_->wake.notify_one();
    impl_->worker.join();
}
void CaretLocator::request(quintptr foregroundWindow, QObject* context, Completion done) {
    const auto generation = ++generation_;
    // The QPointer is only examined on the GUI thread, never on the worker.
    const QPointer<QObject> receiver(context);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pending = [this, generation, foregroundWindow, receiver, done = std::move(done)]() mutable {
            if (generation != generation_.load())
                return;
            auto result = impl_->locate(reinterpret_cast<HWND>(foregroundWindow));
            auto trace = impl_->trace;
            if (GetForegroundWindow() != reinterpret_cast<HWND>(foregroundWindow))
                result.reset();
            QMetaObject::invokeMethod(this,
                [this, generation, receiver, result, trace = std::move(trace),
                 done = std::move(done)]() mutable {
                    if (generation == generation_.load() && receiver)
                        done(result, std::move(trace));
                }, Qt::QueuedConnection);
        };
    }
    impl_->wake.notify_one();
}
std::optional<QRect> CaretLocator::Impl::locate(HWND foreground) const {
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    trace = SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) && apartment == APTTYPE_MTA
                ? QStringLiteral("caretThread=MTA ") : QStringLiteral("caretThread=unavailable ");
    RECT window{};
    GUITHREADINFO thread{sizeof(GUITHREADINFO)};
    if (!foreground || GetForegroundWindow() != foreground || !GetWindowRect(foreground, &window) ||
        !GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &thread))
        return std::nullopt;
    // A real caret is about one text line tall and lies inside the foreground
    // window; Chromium and WinUI park a dummy caret at their client origin.
    const auto fgThread = GetWindowThreadProcessId(foreground, nullptr);
    std::array<wchar_t, 64> className{};
    GetClassNameW(foreground, className.data(), static_cast<int>(className.size()));
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    std::array<wchar_t, MAX_PATH> image{};
    DWORD length = static_cast<DWORD>(image.size());
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    QString program;
    if (process && QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
        program = QString::fromWCharArray(image.data(), length).section(u'\\', -1);
    trace += QStringLiteral("caret: %1 class=%2").arg(program, QString::fromWCharArray(className.data()));
    std::array<wchar_t, 64> focusClass{};
    if (thread.hwndFocus)
        GetClassNameW(thread.hwndFocus, focusClass.data(), static_cast<int>(focusClass.size()));
    trace += QStringLiteral(" focus=%1 caretWindow=%2 rcCaret=%3,%4,%5,%6")
                        .arg(QString::fromWCharArray(focusClass.data()))
                        .arg(thread.hwndCaret ? QStringLiteral("yes") : QStringLiteral("no"))
                        .arg(thread.rcCaret.left).arg(thread.rcCaret.top)
                        .arg(thread.rcCaret.right).arg(thread.rcCaret.bottom);
    const auto plausible = [&](const std::optional<QRect>& caret) {
        if (!caret || caret->height() < 4 || caret->height() > 200)
            return false;
        const POINT center{caret->center().x(), caret->center().y()};
        if (!PtInRect(&window, center))
            return false;
        if (const auto origin = thread.hwndCaret ? clientToPhysical(thread.hwndCaret, {0, 0})
                                                 : std::nullopt;
            origin && qAbs(caret->left() - origin->x) <= 1 && qAbs(caret->top() - origin->y) <= 1)
            return false;
        return true;
    };
    // Cheap native sources first; UI Automation is the slowest and goes last.
    const auto attempt = [&](const char* source, std::optional<QRect> caret) {
        if (caret)
            caret = withLineHeight(*caret);
        const bool used = plausible(caret);
        trace += QStringLiteral(" %1=%2").arg(QLatin1StringView(source),
            !caret ? QStringLiteral("none")
                   : QStringLiteral("%1,%2,%3x%4%5")
                         .arg(caret->x()).arg(caret->y()).arg(caret->width()).arg(caret->height())
                         .arg(used ? QStringLiteral("(used)") : QStringLiteral("(rejected)")));
        return used ? caret : std::nullopt;
    };
    if (auto caret = attempt("win32", win32(thread)))
        return caret;
    if (auto caret = attempt("attached", attachedCaret(fgThread, thread.hwndCaret)))
        return caret;
    if (auto caret = attempt("msaa", msaa(thread)))
        return caret;
    if (auto caret = attempt("ime", imeCaret(fgThread)))
        return caret;
    if (auto caret = attempt("uia", uia()))
        return caret;
    if (auto caret = attempt("focus", focusedEditable(thread)))
        return caret;
    return std::nullopt;
}
} // namespace mt
