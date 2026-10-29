#include <windows.h>
#include "GestureActions.h"
#include "InputTag.h"
#include "infrastructure/i18n/I18n.h"
#include <QFileInfo>
#include <QKeySequence>
#include <array>
#include <vector>
#include <wil/resource.h>

namespace mt {
namespace {
bool desktop(WindowTarget target) {
    const auto window = reinterpret_cast<HWND>(target.handle);
    if (!window) return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    return window == GetShellWindow() || std::wstring_view(name) == L"WorkerW" ||
           std::wstring_view(name) == L"Progman";
}
bool restoreCommand(const QString& command) {
    return command == QStringLiteral("maximize") || command == QStringLiteral("restore") ||
           command == QStringLiteral("toggle_maximize");
}
bool gestureForeground(WindowTarget target) {
    if (!DesktopServices::validTarget(target)) return false;
    const auto foreground = GetForegroundWindow();
    const auto expected = reinterpret_cast<HWND>(target.handle);
    return foreground == expected || GetAncestor(foreground, GA_ROOTOWNER) == expected;
}
bool activateGestureTarget(WindowTarget target) {
    return gestureForeground(target) || (DesktopServices::activate(target) && gestureForeground(target));
}
WORD virtualKey(int key) {
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        return WORD(key);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return WORD(VK_F1 + key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Backspace: return VK_BACK;
    default: return 0;
    }
}
bool extended(WORD vk) {
    return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE ||
           vk == VK_LWIN || (vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE);
}
std::expected<void, QString> sendChord(WindowTarget target, const std::vector<WORD>& keys) {
    if (!gestureForeground(target))
        return std::unexpected(tx(u"目标窗口已改变，手势已取消"));
    if (!DesktopServices::modifiersReleased())
        return std::unexpected(tx(u"请松开修饰键后再执行手势"));
    std::vector<INPUT> events;
    for (auto key : keys) {
        if (GetAsyncKeyState(key) & 0x8000)
            return std::unexpected(tx(u"请松开修饰键后再执行手势"));
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = key;
        event.ki.dwFlags = extended(key) ? KEYEVENTF_EXTENDEDKEY : 0;
        event.ki.dwExtraInfo = ownInputTag;
        events.push_back(event);
    }
    for (size_t index = keys.size(); index > 0; --index) {
        auto release = events[index - 1];
        release.ki.dwFlags |= KEYEVENTF_KEYUP;
        events.push_back(release);
    }
    if (SendInput(UINT(events.size()), events.data(), sizeof(INPUT)) != events.size()) {
        SendInput(UINT(keys.size()), events.data() + keys.size(), sizeof(INPUT));
        return std::unexpected(tx(u"系统拒绝手势动作，目标可能以管理员权限运行"));
    }
    return {};
}
} // namespace
GestureActions::GestureActions()
    : marker_(QStringLiteral("mt.Gesture.Minimized.%1.%2")
                  .arg(GetCurrentProcessId()).arg(quintptr(this), 0, 16).toStdWString()) {}
GestureActions::~GestureActions() { forget(); }
void GestureActions::forget() {
    const auto window = reinterpret_cast<HWND>(lastMinimized_.handle);
    if (window && GetPropW(window, marker_.c_str()) == static_cast<const void*>(this))
        RemovePropW(window, marker_.c_str());
    lastMinimized_ = {};
}
bool GestureActions::savedWindowValid() const {
    const auto window = reinterpret_cast<HWND>(lastMinimized_.handle);
    return lastMinimized_.processStarted && DesktopServices::validTarget(lastMinimized_) &&
           GetPropW(window, marker_.c_str()) == static_cast<const void*>(this) && IsIconic(window);
}
bool GestureActions::available(const GestureAction& action, WindowTarget target) const {
    if (action.kind != GestureAction::Kind::Command) return true;
    if (action.value == QStringLiteral("none")) return false;
    if (!desktop(target)) return true;
    if (restoreCommand(action.value)) return savedWindowValid();
    return action.value != QStringLiteral("minimize") && action.value != QStringLiteral("close_window");
}
std::expected<void, QString> GestureActions::execute(const GestureAction& action, WindowTarget target,
                                                   quintptr foregroundAtStart) {
    if (!DesktopServices::validTarget(target) ||
        reinterpret_cast<quintptr>(GetForegroundWindow()) != foregroundAtStart)
        return std::unexpected(tx(u"目标窗口已改变，手势已取消"));
    if (!available(action, target)) {
        if (desktop(target) && restoreCommand(action.value)) forget();
        return {}; // no action is a normal gesture outcome, not a modal error
    }
    if (action.kind == GestureAction::Kind::Shortcut) {
        auto parsed = GestureAction::parse(action.encode());
        if (!parsed) return std::unexpected(parsed.error());
        const auto key = QKeySequence::fromString(parsed->value, QKeySequence::PortableText)[0];
        std::vector<WORD> keys;
        const auto mods = key.keyboardModifiers();
        if (mods & Qt::ControlModifier) keys.push_back(VK_CONTROL);
        if (mods & Qt::AltModifier) keys.push_back(VK_MENU);
        if (mods & Qt::ShiftModifier) keys.push_back(VK_SHIFT);
        if (mods & Qt::MetaModifier) keys.push_back(VK_LWIN);
        keys.push_back(virtualKey(key.key()));
        if (!activateGestureTarget(target))
            return std::unexpected(tx(u"目标窗口已改变，手势已取消"));
        return sendChord(target, keys);
    }
    const auto& command = action.value;
    if (action.kind != GestureAction::Kind::Command)
        return std::unexpected(tx(u"手势动作配置格式无效"));
    if (command == QStringLiteral("none")) return {};
    if (desktop(target) && restoreCommand(command)) {
        // Consume the record once. Never operate on a recycled/closed or already
        // restored window, and never substitute an arbitrary foreground window.
        const auto saved = lastMinimized_;
        forget();
        const auto window = reinterpret_cast<HWND>(saved.handle);
        const int show = command == QStringLiteral("restore") ? SW_RESTORE : SW_MAXIMIZE;
        if (!ShowWindowAsync(window, show))
            return std::unexpected(tx(u"系统拒绝手势动作，目标可能以管理员权限运行"));
        SetForegroundWindow(window);
        return {};
    }
    if (!restoreCommand(command) && command != QStringLiteral("minimize") &&
        command != QStringLiteral("close_window") && !activateGestureTarget(target))
        return std::unexpected(tx(u"目标窗口已改变，手势已取消"));
    if (command == QStringLiteral("scroll_top")) return sendChord(target, {VK_CONTROL, VK_HOME});
    if (command == QStringLiteral("scroll_bottom")) return sendChord(target, {VK_CONTROL, VK_END});
    if (command == QStringLiteral("copy")) return sendChord(target, {VK_CONTROL, 'C'});
    if (command == QStringLiteral("paste")) return sendChord(target, {VK_CONTROL, 'V'});
    if (command == QStringLiteral("volume_up")) return sendChord(target, {VK_VOLUME_UP});
    if (command == QStringLiteral("volume_down")) return sendChord(target, {VK_VOLUME_DOWN});
    if (command == QStringLiteral("volume_mute")) return sendChord(target, {VK_VOLUME_MUTE});
    if (command == QStringLiteral("media_play")) return sendChord(target, {VK_MEDIA_PLAY_PAUSE});
    const auto window = reinterpret_cast<HWND>(target.handle);
    if (command == QStringLiteral("close_window")) {
        if (!PostMessageW(window, WM_CLOSE, 0, 0))
            return std::unexpected(tx(u"系统拒绝手势动作，目标可能以管理员权限运行"));
        return {};
    }
    int show = 0;
    if (command == QStringLiteral("minimize")) show = SW_MINIMIZE;
    else if (command == QStringLiteral("maximize")) show = SW_MAXIMIZE;
    else if (command == QStringLiteral("restore")) show = SW_RESTORE;
    else if (command == QStringLiteral("toggle_maximize")) show = IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE;
    else return std::unexpected(tx(u"未知手势动作：%1").arg(command));
    if (show == SW_MINIMIZE) {
        forget();
        if (SetPropW(window, marker_.c_str(), reinterpret_cast<HANDLE>(this))) {
            lastMinimized_ = target;
            lastMinimized_.focusedControl = 0;
        }
    }
    if (!ShowWindowAsync(window, show)) {
        if (show == SW_MINIMIZE) forget();
        return std::unexpected(tx(u"系统拒绝手势动作，目标可能以管理员权限运行"));
    }
    return {};
}
std::expected<QString, QString> gestureProgramDirectory(WindowTarget target) {
    if (!DesktopServices::validTarget(target))
        return std::unexpected(tx(u"目标窗口已改变，手势已取消"));
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.processId));
    std::wstring path(32768, L'\0');
    DWORD size = DWORD(path.size());
    if (!process || !QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return std::unexpected(tx(u"无法读取目标程序路径"));
    path.resize(size);
    return QFileInfo(QString::fromStdWString(path)).absolutePath();
}
} // namespace mt
