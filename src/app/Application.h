#pragma once
#include "infrastructure/settings/Settings.h"
#include "platform/windows/DesktopServices.h"
#include "platform/windows/EdgeWatcher.h"
#include "platform/windows/ShellJobs.h"
#include "ui/components/Theme.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>
#include <memory>
class QMenu;
namespace mt {
class ClipboardService;
struct ClipboardRecord;
class ClipboardPanel;
class LauncherService;
class LauncherPanel;
class SettingsDialog;
class ScreenLockService;
class GestureController;
class UpdateService;
class Application final : public QObject {
    Q_OBJECT
  public:
    Application(QString dataRoot, bool preview, DesktopServices& desktop);
    ~Application() override;
    std::expected<void, QString> start();
    void showLauncher();
    void showClipboard();
    void showSettings();
    void lockScreen();
    void saveEvidence(const QString& folder);

  private:
    friend class ClipboardIntegrationTests;
    void saveEvidenceRenders(const QString& folder);
    using SettingsCompletion = std::function<void(std::expected<void, QString>)>;
    void applySettings(const Settings& next, SettingsCompletion done = {});
    QMap<int, QString> shortcuts(const Settings& settings) const;
    ClipboardPanel& clipboardPanel();
    LauncherPanel& launcherPanel();
    void error(const QString& message);
    // Diagnostic line in mt.log only (no tray message).
    void trace(const QString& message) const;
    void launch(LaunchItem item);
    void import(QStringList paths, QString category, int slot);
    void paste(ClipboardRecord record, bool plainText, WindowTarget target);
    void seedPreview();
    void previewOpacity(int launcher, int clipboard);
    void buildTrayMenu();
    void openRecord(const ClipboardRecord& record);
    void clearHistory();
    void importHopy(const QString& folder);
    void importHLaunch(const QString& folder);
    void applyEdgeActivation();
    // Shows the launcher against the screen edge or corner that triggered it.
    void showLauncherAtEdge(EdgeZone zone);
    void applyClipboardBehavior();
    void presentClipboard(std::optional<QRect> caret);
    // "auto" resolves to the current Windows app theme.
    QString effectiveTheme() const;
    void refreshUpdateStatus();
    void installUpdate();
    SettingsStore store_;
    Settings settings_;
    bool preview_;
    DesktopServices& desktop_;
    Theme theme_;
    ShellJobs shellJobs_;
    EdgeWatcher edge_;
    std::unique_ptr<ClipboardService> clipboard_;
    std::unique_ptr<LauncherService> launcher_;
    std::unique_ptr<ScreenLockService> screenLock_;
    std::unique_ptr<GestureController> gestures_;
    std::unique_ptr<UpdateService> updates_;
    bool updateInstalling_ = false;
    QString notifiedUpdateVersion_;
    QPointer<QObject> pasteOperation_;
    std::unique_ptr<ClipboardPanel> clipboardPanel_;
    std::unique_ptr<LauncherPanel> launcherPanel_;
    QPointer<SettingsDialog> settingsDialog_;
    std::unique_ptr<QMenu> menu_;
    QSystemTrayIcon tray_;
    WindowTarget lastTarget_;
    WindowTarget pasteTarget_;
    quint64 pasteGeneration_ = 0;
    std::optional<quint64> clipboardOpening_;
    QElapsedTimer clipboardShown_;
    QStringList errors_;
    bool settingsBusy_ = false;
};
} // namespace mt
