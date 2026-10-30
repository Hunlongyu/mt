#pragma once
#include <QJsonObject>
#include <QString>
#include <expected>

namespace mt {
class LauncherCatalog;
struct Settings {
    QString theme = QStringLiteral("dark"); // dark | light | auto (follow Windows)
    bool startHidden = true;
    bool autoCheckUpdates = true;
    bool autoStart = false;
    bool adminSupport = false;
    bool clipboardEnabled = true;
    bool launcherEnabled = true;
    bool gesturesEnabled = false;
    bool lockEnabled = false;
    bool lockPaused = false;
    bool clipboardPaused = false;
    bool maskSensitive = true;
    int historyLimit = 500;  // ordinary records shown in the panel
    int storageLimit = 1000; // ordinary records kept; favorites and pinned are never pruned
    int launcherOpacity = 95;
    int clipboardOpacity = 95;
    QString clipboardHotkey = QStringLiteral("Alt+C");
    bool pasteOnConfirm = true; // false: confirming only copies to the clipboard
    bool clipboardSuppressFullscreen = true;
    bool hoverPreview = true;
    bool spacePreview = true;
    QString previewSide = QStringLiteral("left");       // left | right
    QString clipboardPlacement = QStringLiteral("cursor"); // fallback without a caret: cursor | center
    QString openKey = QStringLiteral("O");               // empty disables the key
    QString openMouseButton = QStringLiteral("middle");  // middle | right | none
    QString launcherHotkey = QStringLiteral("Alt+Space");
    QString lockHotkey = QStringLiteral("Alt+L");
    bool launcherPinned = false;
    // Launcher grid; the window size follows (3–20 each).
    int launcherColumns = 4;
    int launcherRows = 7;
    // Screen-edge activation (off by default; show only, never hides the panel).
    bool launcherEdgeEnabled = false;
    QStringList launcherEdgeZones{QStringLiteral("left")};
    QString launcherEdgeMode = QStringLiteral("outer"); // outer | every (monitor seams too)
    bool launcherEdgeSuppressFullscreen = true;
    QStringList launcherEdgeBlocklist;
    QStringList launcherEdgeAllowlist;
    bool requirePassword = false;
    QString protectedPassword;
    int gestureThreshold = 20;
    int gestureTimeout = 1000;
    QString gestureTrigger = QStringLiteral("right");
    bool gestureTrail = true;
    bool gestureShowName = true;
    bool disableGesturesFullscreen = true;
    QStringList excludedProcesses;
    QStringList gestureAllowlist;
    QJsonObject gestureActions;
    QJsonObject gestureApplicationActions;
    QJsonObject extra;
    static Settings defaults();
    static std::expected<Settings, QString> decode(const QByteArray& data);
    QByteArray encode() const;
    std::expected<void, QString> validate() const;
};

class SettingsStore {
  public:
    explicit SettingsStore(QString root);
    std::expected<Settings, QString> load();
    std::expected<void, QString> save(const Settings& settings) const;
    std::expected<void, QString> saveLauncherConfiguration(
        const Settings&, const QString& catalogPath, const LauncherCatalog& before,
        const LauncherCatalog& after) const;
    std::expected<void, QString> recoverLauncherConfiguration() const;
    QString root() const {
        return root_;
    }

  private:
    QString root_;
    bool writable_ = true;
};
} // namespace mt
