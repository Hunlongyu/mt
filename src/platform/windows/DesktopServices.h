#pragma once
#include "modules/launcher/LauncherCatalog.h"
#include <QFont>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <expected>
#include <memory>
#include <optional>
#include <functional>

namespace mt {
struct WindowTarget {
    quintptr handle = 0;
    quint32 processId = 0;
    quint64 processStarted = 0;
    quintptr focusedControl = 0;
};
class DesktopServices final : public QObject {
    Q_OBJECT
  public:
    explicit DesktopServices(const QString& identity, QObject* parent = nullptr);
    ~DesktopServices() override;
    bool primaryInstance() const;
    bool valid() const;
    bool notifyPrimary() const;
    // Strict mode (settings changes) is all-or-nothing and restores the old set on
    // failure. Best-effort mode (startup) keeps every key that registers and
    // reports only the conflicting ones, so one busy key never disables the rest.
    std::expected<void, QString> setHotkeys(const QMap<int, QString>& keys,
                                            bool bestEffort = false);
    static WindowTarget foreground();
    static WindowTarget windowTarget(quintptr handle);
    static QFont systemUiFont();
    static bool clientAnimationsEnabled();
    static bool validTarget(WindowTarget target);
    static QString processName(WindowTarget target);
    static bool isForeground(WindowTarget target);
    static bool activate(WindowTarget target);
    // Starts accessibility-based caret lookup (UI Automation registers this
    // process as an assistive client so Chromium/Electron expose their caret).
    void enableCaretTracking();
    // Completes on the GUI thread. Native/COM queries run on the MTA worker;
    // destroying context or issuing a newer request discards this completion.
    void requestCaretRect(QObject* context,
                          std::function<void(std::optional<QRect>, QString)> done);
    // True for a borderless window covering a whole monitor (games, F11, video),
    // as opposed to a maximized window that keeps its resize frame.
    static bool foregroundFullscreen();
    static bool windowFullscreen(quintptr handle);
    // A global Alt+key hotkey leaves the target a lone Alt release, which opens its
    // menu bar and steals the caret; a stray Ctrl tap while Alt is held defeats it.
    static void suppressAltMenu();
    // Relative paths resolve against base; environment variables are expanded.
    static bool revealInExplorer(const QString& path, const QString& base = {});
    static bool clipboardSensitive();
    static bool modifiersReleased();
    static std::expected<void, QString> sendPaste(WindowTarget target);
    static std::expected<void, QString> launch(const LaunchItem& item,
                                               const QString& baseDirectory);
    static std::expected<LaunchItem, QString> importTarget(const QString& path,
                                                           const QString& dataRoot);
    static QImage fileIcon(const QString& path);
    static int runShellHelper();
    // Keyboard and mouse hooks for the non-activating clipboard panel.
    std::expected<void, QString> capturePanelNavigation(bool enabled, bool keepMouse = false);
    // Actual hook state, for diagnostics: keyboard, mouse.
    std::pair<bool, bool> panelInputCaptured() const;
    // While set, mouse side buttons are swallowed and reported as sideButton().
    void setSideButtonCapture(bool enabled);
  Q_SIGNALS:
    void activated();
    void hotkey(int id);
    void panelKey(int key, bool shift, bool control, bool down);
    // A mouse press landed on a window of another process.
    void outsidePress();
    // M4 (back) reports +1, M5 (forward) -1; one signal per press.
    void sideButton(int direction);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mt
