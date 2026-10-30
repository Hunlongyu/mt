#include "Application.h"
#include "BuildInfo.h"
#include "GestureController.h"
#include "infrastructure/i18n/I18n.h"
#include "infrastructure/storage/HLaunchImport.h"
#include "infrastructure/updates/UpdateService.h"
#include "modules/clipboard/ClipboardPanel.h"
#include "modules/clipboard/ClipboardService.h"
#include "modules/clipboard/ClipboardText.h"
#include "modules/launcher/LauncherPanel.h"
#include "modules/launcher/LauncherService.h"
#include "modules/screen_lock/ScreenLockService.h"
#include "modules/screen_lock/UnlockPolicy.h"
#include "platform/windows/AdminBridge.h"
#include "platform/windows/LockPassword.h"
#include "platform/windows/StartupRegistration.h"
#include "platform/windows/UpdateInstaller.h"
#include "ui/components/Placement.h"
#include "ui/shell/SettingsDialog.h"
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <algorithm>

namespace mt {
namespace {
// Draws a simple synthetic mock-up so preview mode has image records without
// reading any screenshot or user file.
QImage previewImage(QSize size, int variant) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    const QColor palettes[][3]{{QColor(0x30, 0x4B, 0x67), QColor(0x4D, 0xAA, 0xFC), QColor(0xE8, 0xED, 0xF2)},
                               {QColor(0x2D, 0x2D, 0x30), QColor(0x7C, 0xC4, 0x7F), QColor(0xF3, 0xF3, 0xF3)},
                               {QColor(0x5B, 0x3A, 0x72), QColor(0xF2, 0xA6, 0x5A), QColor(0xFF, 0xFF, 0xFF)}};
    const auto& c = palettes[variant % 3];
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient background(0, 0, size.width(), size.height());
    background.setColorAt(0, c[0]);
    background.setColorAt(1, c[0].darker(140));
    painter.fillRect(image.rect(), background);
    const int unit = qMax(8, qMin(size.width(), size.height()) / 12);
    painter.setPen(Qt::NoPen);
    painter.setBrush(c[2]);
    painter.setOpacity(0.92);
    painter.drawRoundedRect(QRect(unit, unit, size.width() - 2 * unit, unit * 2), unit / 2, unit / 2);
    painter.setBrush(c[1]);
    painter.setOpacity(1.0);
    painter.drawEllipse(QRect(unit * 2, unit * 4, unit * 3, unit * 3));
    painter.setBrush(c[2]);
    for (int row = 0; row * unit * 2 + unit * 8 < size.height() - unit; ++row) {
        painter.setOpacity(0.25 + 0.1 * (row % 3));
        painter.drawRoundedRect(QRect(unit * 6, unit * 4 + row * unit * 2,
                                      size.width() - unit * 7 - (row % 2) * unit * 3, unit),
                                unit / 3, unit / 3);
    }
    painter.end();
    return image;
}
} // namespace
Application::Application(QString root, bool preview, DesktopServices& desktop)
    : store_(std::move(root)), preview_(preview), desktop_(desktop), theme_(this), tray_(this) {}
Application::~Application() {
    updates_.reset();
    gestures_.reset();
    // stop() emits status while destroying the helper. Disconnect UI observers
    // before reset() clears the owning pointer.
    if (admin_)
        admin_->disconnect(this);
    admin_.reset();
    if (screenLock_)
        screenLock_->shutdown();
    (void)desktop_.capturePanelNavigation(false);
    (void)desktop_.setHotkeys({});
    tray_.hide();
    shellJobs_.shutdown();
    delete settingsDialog_.data();
    clipboardPanel_.reset();
    launcherPanel_.reset();
    clipboard_.reset();
}
QMap<int, QString> Application::shortcuts(const Settings& s) const {
    QMap<int, QString> keys;
    if (s.clipboardEnabled)
        keys.insert(1, s.clipboardHotkey);
    if (s.launcherEnabled)
        keys.insert(2, s.launcherHotkey);
    if (s.lockEnabled && !s.lockPaused)
        keys.insert(3, s.lockHotkey);
    return keys;
}
std::expected<void, QString> Application::start() {
    auto loaded = store_.load();
    if (!loaded)
        return std::unexpected(loaded.error());
    settings_ = std::move(*loaded);
    startup_ = std::make_unique<StartupRegistration>(QCoreApplication::applicationFilePath(),
                                                     store_.root());
    admin_ = std::make_unique<AdminBridge>();
    connect(admin_.get(), &AdminBridge::failed, this, &Application::error);
    connect(admin_.get(), &AdminBridge::changed, this, [this] {
        if (settingsDialog_)
            settingsDialog_->setSystemIntegrationStatus(preview_, admin_->status());
    });
    if (!preview_) {
        const auto registered = startup_->read();
        if (registered)
            settings_.autoStart = *registered && **registered == startup_->command();
        else
            QTimer::singleShot(0, this, [this, message = registered.error()] { error(message); });
    }
    updates_ = std::make_unique<UpdateService>(QString::fromLatin1(TOOLS_VERSION),
                                              QDir(store_.root()).filePath(QStringLiteral("updates")));
    connect(updates_.get(), &UpdateService::changed, this, &Application::refreshUpdateStatus);
    if (!preview_) {
        auto check = [this] {
            if (settings_.autoCheckUpdates && !updateInstalling_ &&
                !(screenLock_ && screenLock_->active()) &&
                updates_->state() != UpdateService::State::Ready)
                updates_->check();
        };
        QTimer::singleShot(30000, this, check);
        auto* timer = new QTimer(this);
        timer->setInterval(24 * 60 * 60 * 1000);
        connect(timer, &QTimer::timeout, this, check);
        timer->start();
    }
    theme_.apply(effectiveTheme());
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (settings_.theme == QStringLiteral("auto"))
            theme_.apply(effectiveTheme());
    });
    launcher_ = std::make_unique<LauncherService>(
        QDir(store_.root()).filePath(QStringLiteral("launcher.json")), this);
    if (auto result = launcher_->load(); !result)
        return result;
    clipboard_ = std::make_unique<ClipboardService>(
        QDir(store_.root()).filePath(QStringLiteral("clipboard.sqlite")), this);
    connect(launcher_.get(), &LauncherService::failed, this, &Application::error);
    // Startup uses the committed catalog's columns. Configuration commits keep
    // this in sync; an interrupted pair was recovered by SettingsStore::load.
    settings_.launcherColumns = launcher_->catalog().columns;
    connect(clipboard_.get(), &ClipboardService::failed, this, &Application::error);
    connect(clipboard_.get(), &ClipboardService::ready, this, [this] {
        if (preview_)
            seedPreview();
    });
    clipboard_->configure(settings_.clipboardEnabled, settings_.clipboardPaused,
                          settings_.storageLimit, settings_.historyLimit);
    screenLock_ = std::make_unique<ScreenLockService>();
    connect(screenLock_.get(), &ScreenLockService::failed, this, &Application::error);
    connect(screenLock_.get(), &ScreenLockService::activeChanged, this, [this](bool active) {
        ++pasteGeneration_;
        if (active) {
            if (gestures_)
                gestures_->blockNow();
            (void)desktop_.capturePanelNavigation(false);
            desktop_.setSideButtonCapture(false);
            if (clipboardPanel_)
                clipboardPanel_->hide();
            if (launcherPanel_)
                launcherPanel_->hide();
            if (settingsDialog_)
                settingsDialog_->close();
            if (menu_)
                menu_->hide();
            shellJobs_.cancelQueuedActions();
        }
        clipboard_->configure(settings_.clipboardEnabled, active || settings_.clipboardPaused,
                              settings_.storageLimit, settings_.historyLimit);
        applyEdgeActivation();
        trace(active ? QStringLiteral("blackout starting") : QStringLiteral("blackout stopped"));
    });
    connect(qApp, &QCoreApplication::aboutToQuit, screenLock_.get(), &ScreenLockService::shutdown);
    gestures_ = std::make_unique<GestureController>(theme_, store_.root(), preview_, [this] {
        return (screenLock_ && screenLock_->active()) || settingsBusy_ || updateInstalling_ ||
               pasteOperation_ || clipboardOpening_.has_value() ||
               (clipboardPanel_ && clipboardPanel_->isVisible()) ||
               QApplication::activePopupWidget() || QApplication::activeModalWidget();
    });
    connect(gestures_.get(), &GestureController::failed, this, &Application::error);
    gestures_->setAdminBridge(admin_.get());
    if (auto result = gestures_->configure(settings_); !result) {
        settings_.gesturesEnabled = false;
        QTimer::singleShot(500, this, [this, message = result.error()] { error(message); });
    }
    buildTrayMenu();
    tray_.setIcon(QIcon(QStringLiteral(":/mt/logo.ico")));
    tray_.setToolTip(
        QStringLiteral("MT %1%2")
            .arg(QString::fromLatin1(TOOLS_VERSION),
                 preview_ ? tx(u" · 隔离预览") : QString{}));
    tray_.show();
    connect(&tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (screenLock_->active())
                    return;
                if (reason == QSystemTrayIcon::Trigger)
                    showLauncher();
                else if (reason == QSystemTrayIcon::Context)
                    menu_->popup(QCursor::pos());
            });
    connect(&desktop_, &DesktopServices::activated, this, [this] {
        if (screenLock_->active())
            return;
        if (!settings_.launcherEnabled) {
            showSettings();
            return;
        }
        auto& panel = launcherPanel();
        if (panel.isVisible()) {
            panel.raise();
            panel.activateWindow();
        } else
            showLauncher();
    });
    connect(&desktop_, &DesktopServices::hotkey, this, [this](int id) {
        if (screenLock_->active())
            return;
        if (id == 3) {
            lockScreen();
            return;
        }
        if (id == 1) {
            const bool visible = clipboardPanel_ && clipboardPanel_->isVisible();
            // Never pop up over a game or fullscreen video; the hotkey still hides.
            if (!visible && settings_.clipboardSuppressFullscreen &&
                DesktopServices::foregroundFullscreen())
                return;
            if (!visible && settings_.adminSupport && admin_->ready()) {
                if (admin_->busy())
                    return;
                const auto target = DesktopServices::foreground();
                const auto generation = pasteGeneration_;
                if (target.handle) {
                    admin_->suppressAltMenu(target, this, [this, target, generation](auto result) {
                        if (generation != pasteGeneration_ ||
                            !DesktopServices::isForeground(target) || screenLock_->active() ||
                            settingsBusy_ || updateInstalling_)
                            return;
                        if (!result)
                            error(result.error());
                        showClipboard();
                    });
                    return;
                }
            }
            if (!visible)
                DesktopServices::suppressAltMenu();
            showClipboard();
        }
        if (id == 2)
            showLauncher();
    });
    connect(&desktop_, &DesktopServices::panelKey, this,
            [this](int key, bool shift, bool ctrl, bool down) {
                if (clipboardPanel_ && clipboardPanel_->isVisible() &&
                    !clipboardPanel_->searching())
                    clipboardPanel_->handleKey(key, shift, ctrl, down);
            });
    connect(&desktop_, &DesktopServices::outsidePress, this, [this] {
        // Ignore the press burst that can arrive while the panel is appearing.
        if (clipboardPanel_ && clipboardPanel_->isVisible() && clipboardShown_.isValid() &&
            clipboardShown_.elapsed() > 200)
            clipboardPanel_->hide();
    });
    connect(&edge_, &EdgeWatcher::triggered, this, &Application::showLauncherAtEdge);
    applyEdgeActivation();
    connect(&desktop_, &DesktopServices::sideButton, this, [this](int direction) {
        if (clipboardPanel_)
            clipboardPanel_->sideScroll(direction);
    });
    if (!preview_) {
        if (auto result = desktop_.setHotkeys(shortcuts(settings_), true); !result)
            QTimer::singleShot(500, this, [this, message = result.error()] { error(message); });
    }
    auto* focusTimer = new QTimer(this);
    focusTimer->setInterval(150);
    connect(focusTimer, &QTimer::timeout, this, [this] {
        const auto target = DesktopServices::foreground();
        if (!target.handle)
            return;
        if (clipboardPanel_ && clipboardPanel_->isVisible() && pasteTarget_.handle &&
            target.handle != pasteTarget_.handle)
            clipboardPanel_->hide();
        lastTarget_ = target;
    });
    if (!preview_)
        focusTimer->start();
    clipboard_->start(!preview_);
    if (!preview_)
        desktop_.enableCaretTracking();
    if (preview_) {
        showLauncher();
        showClipboard();
    } else if (!settings_.startHidden && settings_.launcherEnabled)
        showLauncher();
    if (!preview_ && settings_.adminSupport)
        QTimer::singleShot(0, this, [this] {
            admin_->start([this](auto result) {
                if (!result)
                    error(result.error());
            });
        });
    return {};
}
void Application::applySettings(const Settings& next, SettingsCompletion done) {
    if (!done) done = [this](auto result) { if (!result) error(result.error()); };
    if (settingsBusy_) { done(std::unexpected(tx(u"设置正在保存，请稍后重试"))); return; }
    if (admin_ && admin_->busy()) {
        done(std::unexpected(tx(u"管理员操作正在进行，请稍后保存设置")));
        return;
    }
    if (auto valid = next.validate(); !valid) {
        done(valid);
        return;
    }
    if (screenLock_->active()) {
        done(std::unexpected(tx(u"请先解除黑屏遮挡再修改设置")));
        return;
    }
    if (!preview_ && next.adminSupport && !settings_.adminSupport && !admin_->ready()) {
        settingsBusy_ = true;
        admin_->start([this, next, done = std::move(done)](auto result) mutable {
            settingsBusy_ = false;
            if (!result)
                done(result);
            else
                applySettings(next, std::move(done));
        });
        return;
    }
    const bool hotkeysChanged = shortcuts(next) != shortcuts(settings_);
    const auto prepare = [&]() -> std::expected<void, QString> {
    if (auto valid = next.validate(); !valid)
        return valid;
    if (screenLock_->active())
        return std::unexpected(tx(u"请先解除黑屏遮挡再修改设置"));
    if (next.requirePassword) {
        auto plain = revealLockPassword(next.protectedPassword);
        if (!plain)
            return std::unexpected(plain.error());
        const bool valid = UnlockPolicy::validPassword(*plain);
        plain->fill(QChar{});
        if (!valid)
            return std::unexpected(tx(u"密码须为 1–128 位英文字母或数字，区分大小写"));
    }
    if (!preview_ && hotkeysChanged) {
        if (auto keys = desktop_.setHotkeys(shortcuts(next)); !keys)
            return keys;
    }
    if (auto result = gestures_->configure(next); !result) {
        if (!preview_ && hotkeysChanged)
            (void)desktop_.setHotkeys(shortcuts(settings_));
        return result;
    }
    return {};
    };
    if (auto result = prepare(); !result) {
        if (!settings_.adminSupport)
            admin_->stop();
        done(result);
        return;
    }
    const bool startupChanged = !preview_ && next.autoStart != settings_.autoStart;
    StartupRegistration::Snapshot startupBefore;
    if (startupChanged) {
        auto prior = startup_->read();
        auto registered = prior ? startup_->setEnabled(next.autoStart)
                                : std::expected<void, QString>{std::unexpected(prior.error())};
        if (!registered) {
            (void)gestures_->configure(settings_);
            if (hotkeysChanged)
                (void)desktop_.setHotkeys(shortcuts(settings_));
            if (!settings_.adminSupport)
                admin_->stop();
            done(registered);
            return;
        }
        startupBefore = *prior;
    }
    settingsBusy_ = true;
    launcher_->configure(
        store_, next,
        [this, next, hotkeysChanged, startupChanged, startupBefore,
         done = std::move(done)](auto saved) {
            settingsBusy_ = false;
            if (!saved) {
                if (startupChanged) {
                    auto restored = startup_->restore(
                        startupBefore, next.autoStart
                                           ? StartupRegistration::Snapshot{startup_->command()}
                                           : StartupRegistration::Snapshot{});
                    if (!restored)
                        error(restored.error());
                }
                if (!settings_.adminSupport)
                    admin_->stop();
                if (auto restored = gestures_->configure(settings_); !restored)
                    error(restored.error());
                if (!preview_ && hotkeysChanged) {
                    if (auto restored = desktop_.setHotkeys(shortcuts(settings_)); !restored)
                        error(restored.error());
                }
                done(saved);
                return;
            }
            settings_ = next;
            if (!next.adminSupport)
                admin_->stop();
            if (!next.clipboardEnabled) {
                clipboardOpening_.reset();
                ++pasteGeneration_;
            }
            previewOpacity(next.launcherOpacity, next.clipboardOpacity);
            theme_.apply(effectiveTheme());
            clipboard_->configure(next.clipboardEnabled, next.clipboardPaused, next.storageLimit,
                                  next.historyLimit);
            applyClipboardBehavior();
            applyEdgeActivation();
            if (launcherPanel_)
                launcherPanel_->setGrid(launcher_->catalog().columns, next.launcherRows);
            if (clipboardPanel_) {
                clipboardPanel_->setMaskSensitive(next.maskSensitive);
                clipboardPanel_->setPaused(next.clipboardPaused);
                if (!next.clipboardEnabled)
                    clipboardPanel_->hide();
            }
            if (launcherPanel_) {
                launcherPanel_->setPinned(next.launcherPinned);
                if (!next.launcherEnabled)
                    launcherPanel_->hide();
            }
            done({});
        });
}
void Application::trace(const QString& message) const {
    // Tray balloons vanish; keep a small local record for diagnosis.
    QFile log(QDir(store_.root()).filePath(QStringLiteral("mt.log")));
    if (log.size() > 1024 * 1024)
        log.remove();
    if (log.open(QIODevice::Append | QIODevice::Text))
        log.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + u' ' + message + u'\n')
                      .toUtf8());
}
void Application::error(const QString& message) {
    errors_.append(message);
    qWarning().noquote() << message;
    trace(message);
    tray_.showMessage(QStringLiteral("MT"), message, QSystemTrayIcon::Warning, 5000);
}
LauncherPanel& Application::launcherPanel() {
    if (!launcherPanel_) {
        launcherPanel_ = std::make_unique<LauncherPanel>(theme_, *launcher_);
        launcherPanel_->setAnimated(DesktopServices::clientAnimationsEnabled());
        launcherPanel_->setPanelOpacity(settings_.launcherOpacity);
        launcherPanel_->setPinned(settings_.launcherPinned);
        launcherPanel_->setGrid(launcher_->catalog().columns, settings_.launcherRows);
        connect(launcherPanel_.get(), &LauncherPanel::iconRequested, this,
                [this](const QString& id, const QString& source) {
                    shellJobs_.icon(id, source, store_.root(), [this, id, source](QImage image) {
                        if (launcherPanel_)
                            launcherPanel_->acceptIcon(id, source, image);
                    });
                });
        launcherPanel_->refresh();
        connect(launcherPanel_.get(), &LauncherPanel::settingsRequested, this,
                &Application::showSettings);
        connect(launcherPanel_.get(), &LauncherPanel::launchRequested, this, &Application::launch);
        connect(launcherPanel_.get(), &LauncherPanel::importRequested, this, &Application::import);
        connect(launcherPanel_.get(), &LauncherPanel::revealRequested, this,
                [this](const QString& target) {
                    if (preview_) {
                        tray_.showMessage(tx(u"隔离预览"),
                                          tx(u"预览模式未启动程序或打开链接。"));
                        return;
                    }
                    shellJobs_.reveal(target, store_.root(), [this](auto result) {
                        if (!result) error(result.error());
                    });
                });
        connect(launcherPanel_.get(), &LauncherPanel::pinnedChanged, this, [this](bool value) {
            auto next = settings_;
            next.launcherPinned = value;
            applySettings(next, [this](auto result) {
                if (!result) {
                    if (launcherPanel_) launcherPanel_->setPinned(settings_.launcherPinned);
                    error(result.error());
                }
            });
        });
    }
    return *launcherPanel_;
}
ClipboardPanel& Application::clipboardPanel() {
    if (!clipboardPanel_) {
        clipboardPanel_ =
            std::make_unique<ClipboardPanel>(theme_, DesktopServices::clientAnimationsEnabled());
        clipboardPanel_->setAnimated(DesktopServices::clientAnimationsEnabled());
        clipboardPanel_->setPanelOpacity(settings_.clipboardOpacity);
        clipboardPanel_->setMaskSensitive(settings_.maskSensitive);
        clipboardPanel_->setPaused(settings_.clipboardPaused);
        applyClipboardBehavior();
        connect(clipboardPanel_.get(), &ClipboardPanel::previewVisibleChanged, this,
                [this](bool visible) { desktop_.setSideButtonCapture(visible); });
        connect(clipboard_.get(), &ClipboardService::recordsChanged, clipboardPanel_.get(),
                &ClipboardPanel::setRecords);
        connect(clipboardPanel_.get(), &ClipboardPanel::queryChanged, clipboard_.get(),
                &ClipboardService::refresh);
        connect(clipboardPanel_.get(), &ClipboardPanel::toggleFlagRequested, clipboard_.get(),
                &ClipboardService::toggleFlag);
        connect(clipboardPanel_.get(), &ClipboardPanel::removeRequested, clipboard_.get(),
                &ClipboardService::remove);
        connect(clipboardPanel_.get(), &ClipboardPanel::pasteRequested, this,
                [this](const ClipboardRecord& summary, bool plain) {
                    const auto target = pasteTarget_;
                    const auto generation = ++pasteGeneration_;
                    clipboard_->retrieve(summary.id,
                                         [this, target, generation, plain](ClipboardRecord record) {
                                             if (generation == pasteGeneration_)
                                                 paste(std::move(record), plain, target);
                                         });
                });
        connect(clipboardPanel_.get(), &ClipboardPanel::previewRequested, this,
                [this](const QString& id, quint64 token) {
                    clipboard_->retrieve(id, [this, token](ClipboardRecord record) {
                        if (clipboardPanel_ && !screenLock_->active())
                            clipboardPanel_->displayPreview(record, token);
                    });
                });
        connect(clipboardPanel_.get(), &ClipboardPanel::settingsRequested, this,
                &Application::showSettings);
        connect(clipboardPanel_.get(), &ClipboardPanel::dismissed, this, [this] {
            ++pasteGeneration_;
            (void)desktop_.capturePanelNavigation(false);
        });
        connect(clipboardPanel_.get(), &ClipboardPanel::searchModeChanged, this,
                [this](bool searching) {
                    if (preview_)
                        return;
                    if (searching) {
                        if (auto result = desktop_.capturePanelNavigation(false, true); !result)
                            error(result.error());
                    }
                    else if (clipboardPanel_->isVisible()) {
                        if (clipboardPanel_->isActiveWindow())
                            (void)DesktopServices::activate(pasteTarget_);
                        if (auto result = desktop_.capturePanelNavigation(true); !result)
                            error(result.error());
                    }
                });
        connect(clipboardPanel_.get(), &ClipboardPanel::pauseRequested, this, [this](bool paused) {
            auto next = settings_;
            next.clipboardPaused = paused;
            applySettings(next);
        });
        connect(clipboardPanel_.get(), &ClipboardPanel::openRequested, this,
                [this](const QString& id) {
                    const auto generation = pasteGeneration_;
                    clipboard_->retrieve(id, [this, generation](ClipboardRecord record) {
                        if (generation == pasteGeneration_)
                            openRecord(record);
                    });
                });
    }
    return *clipboardPanel_;
}
void Application::showLauncher() {
    if (!preview_ && clipboardOpening_) {
        clipboardOpening_.reset();
        ++pasteGeneration_;
    }
    if (gestures_) gestures_->blockNow();
    if (screenLock_ && screenLock_->active())
        return;
    if (!settings_.launcherEnabled)
        return;
    auto& panel = launcherPanel();
    if (panel.isVisible()) {
        panel.hide();
        return;
    }
    const auto target = DesktopServices::foreground();
    if (target.handle)
        lastTarget_ = target;
    if (!preview_ && clipboardPanel_)
        clipboardPanel_->hide();
    panel.showNear(preview_ ? QPoint(160, 180) : QCursor::pos());
}
void Application::showClipboard() {
    if (gestures_) gestures_->blockNow();
    if (screenLock_ && screenLock_->active())
        return;
    if (!settings_.clipboardEnabled)
        return;
    // A new invocation cancels both delayed writeback and an earlier lookup.
    const bool wasOpening = clipboardOpening_ && *clipboardOpening_ == pasteGeneration_;
    clipboardOpening_.reset();
    ++pasteGeneration_;
    if (auto* timer = qobject_cast<QTimer*>(pasteOperation_.data())) {
        timer->stop();
        timer->deleteLater();
        pasteOperation_.clear();
    }
    if (wasOpening)
        return;
    auto& panel = clipboardPanel();
    if (panel.isVisible()) {
        panel.hide();
        return;
    }
    const auto target = DesktopServices::foreground();
    pasteTarget_ = target.handle ? target : lastTarget_;
    if (!preview_ && launcherPanel_)
        launcherPanel_->hide();
    if (preview_) {
        presentClipboard(std::nullopt);
        return;
    }
    const auto generation = pasteGeneration_;
    clipboardOpening_ = generation;
    desktop_.requestCaretRect(this, [this, generation, target](std::optional<QRect> caret,
                                                              QString diagnostic) {
        if (clipboardOpening_ != generation)
            return;
        clipboardOpening_.reset();
        const auto current = DesktopServices::foreground();
        if (generation != pasteGeneration_ || !settings_.clipboardEnabled ||
            (screenLock_ && screenLock_->active()) || current.handle != target.handle ||
            (target.handle && !DesktopServices::isForeground(target)))
            return;
        trace(diagnostic);
        presentClipboard(caret);
    });
}
void Application::presentClipboard(std::optional<QRect> caret) {
    auto& panel = clipboardPanel();
    QPoint anchor(510, 180);
    if (!preview_) {
        // The caret always wins; without one, use the configured fallback.
        if (caret) {
            auto* screen = QGuiApplication::screenAt(caret->center());
            anchor = screen ? placeBesideCaret(*caret, panel.size(), screen->availableGeometry())
                            : caret->bottomLeft();
        }
        else if (settings_.clipboardPlacement == QStringLiteral("center")) {
            auto* screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            anchor = screen->availableGeometry().center() -
                     QPoint(panel.width() / 2, panel.height() / 2);
        } else
            anchor = QCursor::pos();
    }
    clipboardShown_.start();
    panel.showNear(anchor, false);
    panel.selectNewestOnNextRecords();
    clipboard_->refresh({}, panel.filter());
    if (!preview_) {
        if (auto result = desktop_.capturePanelNavigation(true); !result) {
            error(result.error());
            panel.hide();
        }
    }
}
void Application::previewOpacity(int launcher, int clipboard) {
    if (launcherPanel_)
        launcherPanel_->setPanelOpacity(launcher);
    if (clipboardPanel_)
        clipboardPanel_->setPanelOpacity(clipboard);
}
void Application::showSettings() {
    if (clipboardOpening_) {
        clipboardOpening_.reset();
        ++pasteGeneration_;
    }
    if (gestures_) gestures_->blockNow();
    if (screenLock_ && screenLock_->active())
        return;
    if (settingsDialog_) {
        settingsDialog_->show();
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }
    if (clipboardPanel_)
        clipboardPanel_->hide();
    settingsDialog_ =
        new SettingsDialog(theme_, settings_, store_.root(),
                           [this](const Settings& next, SettingsCompletion done) { applySettings(next, std::move(done)); });
    settingsDialog_->setAttribute(Qt::WA_DeleteOnClose);
    settingsDialog_->setSystemIntegrationStatus(preview_, admin_->status());
    connect(settingsDialog_, &SettingsDialog::panelOpacityPreview, this,
            &Application::previewOpacity);
    connect(settingsDialog_, &SettingsDialog::clearHistoryRequested, this,
            &Application::clearHistory);
    connect(settingsDialog_, &SettingsDialog::importHopyRequested, this, &Application::importHopy);
    connect(settingsDialog_, &SettingsDialog::importHLaunchRequested, this,
            &Application::importHLaunch);
    connect(settingsDialog_, &SettingsDialog::checkUpdatesRequested, this, [this] {
        if (!preview_ && !updateInstalling_ && !screenLock_->active()) updates_->check();
    });
    connect(settingsDialog_, &SettingsDialog::cancelUpdateRequested, this, [this] {
        if (!updateInstalling_) updates_->cancel();
    });
    connect(settingsDialog_, &SettingsDialog::updateRequested, this, [this] {
        if (preview_ || updateInstalling_ || screenLock_->active()) return;
        if (updates_->state() == UpdateService::State::Ready) installUpdate();
        else updates_->download();
    });
    refreshUpdateStatus();
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}
void Application::refreshUpdateStatus() {
    if (!updates_) return;
    if (!preview_ && updates_->state() == UpdateService::State::Available &&
        updates_->release() && notifiedUpdateVersion_ != updates_->release()->version &&
        !(screenLock_ && screenLock_->active())) {
        notifiedUpdateVersion_ = updates_->release()->version;
        tray_.showMessage(QStringLiteral("MT"),
                          QStringLiteral("发现新版本 %1，请在设置 → 关于 MT 中更新。")
                              .arg(notifiedUpdateVersion_));
    }
    if (!settingsDialog_) return;
    const bool enabled = !preview_ && !updateInstalling_;
    settingsDialog_->setUpdateStatus(
        preview_ ? QStringLiteral("隔离预览不联网或安装更新")
                 : updateInstalling_ ? QStringLiteral("正在准备安装，随后自动重启…") : updates_->message(),
        enabled && !updates_->busy(), enabled && !updates_->busy() && updates_->release().has_value(),
        updates_->state() == UpdateService::State::Ready, enabled && updates_->busy());
}
void Application::installUpdate() {
    if (!settingsDialog_ || !updates_->release() || updateInstalling_ || preview_) return;
    if (settingsBusy_ || settingsDialog_->hasUnsavedChanges()) {
        settingsDialog_->setUpdateStatus(QStringLiteral("请先保存或取消设置更改，再安装更新"),
                                         true, true, true, false);
        return;
    }
    auto* prompt = new QMessageBox(QMessageBox::Question, QStringLiteral("安装 MT 更新"),
        QStringLiteral("安装版本 %1 并重启 MT？设置与功能数据将保留。")
            .arg(updates_->release()->version), QMessageBox::Yes | QMessageBox::Cancel, settingsDialog_);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    prompt->setDefaultButton(QMessageBox::Cancel);
    connect(prompt, &QMessageBox::finished, this, [this](int result) {
        if (result != QMessageBox::Yes || updateInstalling_ || screenLock_->active() ||
            settingsBusy_ || (admin_ && admin_->busy()) || !updates_->release() ||
            updates_->state() != UpdateService::State::Ready || !settingsDialog_ ||
            settingsDialog_->hasUnsavedChanges())
            return;
        updateInstalling_ = true;
        refreshUpdateStatus();
        const auto& release = *updates_->release();
        auto prepared = prepareUpdateInstaller(updates_->candidatePath(), release.sha256,
                                               release.bytes, store_.root());
        if (!prepared) {
            updateInstalling_ = false;
            refreshUpdateStatus();
            error(prepared.error());
            return;
        }
        auto* timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this, timer, folder = *prepared, attempts = 0]() mutable {
            QFile failure(QDir(folder).filePath(QStringLiteral("error.txt")));
            if (failure.exists() || ++attempts >= 150) {
                QFile cancelled(QDir(folder).filePath(QStringLiteral("cancelled")));
                if (cancelled.open(QIODevice::WriteOnly)) cancelled.write("cancelled");
                const auto message = failure.open(QIODevice::ReadOnly)
                    ? QString::fromUtf8(failure.read(16384)) : QStringLiteral("更新助手未就绪，当前程序继续运行");
                timer->stop();
                timer->deleteLater();
                updateInstalling_ = false;
                refreshUpdateStatus();
                error(message);
            } else if (QFile::exists(QDir(folder).filePath(QStringLiteral("ready")))) {
                timer->stop();
                timer->deleteLater();
                // The elevated copy uses this same EXE and must release it before
                // the installer can replace the file after our exit.
                if (admin_ && !admin_->stop()) {
                    QFile cancelled(QDir(folder).filePath(QStringLiteral("cancelled")));
                    if (cancelled.open(QIODevice::WriteOnly))
                        cancelled.write("cancelled");
                    updateInstalling_ = false;
                    refreshUpdateStatus();
                    error(QStringLiteral("管理员辅助进程尚未退出，已取消安装，请稍后重试"));
                    return;
                }
                qApp->quit();
            }
        });
        timer->start();
    });
    prompt->open();
}
void Application::launch(LaunchItem item) {
    if (screenLock_ && screenLock_->active())
        return;
    if (preview_) {
        tray_.showMessage(tx(u"隔离预览"),
                          tx(u"预览模式未启动程序或打开链接。"));
        return;
    }
    shellJobs_.launch(item, store_.root(), [this, id = item.id](auto result) {
        if (!result) {
            error(result.error());
            return;
        }
        // Only launches the Shell accepted count towards search ranking.
        if (!id.isEmpty())
            launcher_->recordLaunch(id);
    });
}
void Application::import(QStringList paths, QString category, int slot) {
    if (screenLock_ && screenLock_->active())
        return;
    if (paths.size() > 1000) {
        error(tx(u"一次最多添加 1000 个项目"));
        return;
    }
    shellJobs_.importPaths(paths, store_.root(), [this, category, slot](auto result) {
        if (!result) {
            error(result.error());
            return;
        }
        // Exact duplicates (same target, arguments, folder and elevation) are skipped.
        launcher_->addItems(category, slot, *result, [this](auto skipped) {
            if (skipped && *skipped > 0)
                tray_.showMessage(QStringLiteral("MT"), tx(u"已跳过 %1 个完全相同的启动项").arg(*skipped));
        });
    });
}
void Application::paste(ClipboardRecord record, bool plain, WindowTarget target) {
    if (admin_ && admin_->busy()) {
        error(tx(u"管理员操作正在进行，本次回贴已取消"));
        return;
    }
    if (plain && record.kind == ClipKind::Formats) {
        error(tx(u"该内容没有纯文本格式，请使用普通粘贴"));
        return;
    }
    if (screenLock_ && screenLock_->active())
        return;
    if (preview_) {
        tray_.showMessage(tx(u"隔离预览"),
                          tx(u"预览模式未修改系统剪贴板或发送按键。"));
        return;
    }
    if (!settings_.pasteOnConfirm) {
        // Copy-only confirmation: the record goes to the clipboard and the user pastes.
        clipboardPanel_->hide();
        if (!clipboard_->write(record, plain)) {
            error(tx(u"剪贴板被其他程序占用，未能写入"));
            return;
        }
        clipboard_->touch(record.id);
        return;
    }
    const auto current = DesktopServices::foreground();
    if (!DesktopServices::validTarget(target) ||
        (current.handle && current.handle != target.handle)) {
        error(tx(u"原目标窗口已改变，回贴已取消"));
        return;
    }
    clipboardPanel_->hide();
    if (!DesktopServices::activate(target)) {
        error(tx(u"无法恢复目标窗口焦点，回贴已取消"));
        return;
    }
    const auto generation = ++pasteGeneration_;
    auto* timer = new QTimer(this);
    pasteOperation_ = timer;
    if (gestures_) gestures_->blockNow();
    timer->setInterval(25);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto writeAttempts = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this,
            [this, timer, elapsed, writeAttempts, generation, target, record = std::move(record),
             plain] {
                if (generation != pasteGeneration_ || screenLock_->active() || !settings_.clipboardEnabled ||
                    !DesktopServices::isForeground(target)) {
                    timer->stop();
                    timer->deleteLater();
                    error(tx(u"目标或模块状态已改变，回贴已取消"));
                    return;
                }
                if (!DesktopServices::modifiersReleased()) {
                    if (elapsed->elapsed() < 1500)
                        return;
                    timer->stop();
                    timer->deleteLater();
                    error(tx(u"修饰键仍按下，回贴已取消"));
                    return;
                }
                // Another program can hold the clipboard open briefly; retry the write
                // instead of pasting whatever content was there before.
                if (!clipboard_->write(record, plain)) {
                    if (++*writeAttempts < 8)
                        return;
                    timer->stop();
                    timer->deleteLater();
                    error(tx(u"剪贴板被其他程序占用，回贴已取消"));
                    return;
                }
                timer->stop();
                timer->deleteLater();
                if (settings_.adminSupport && admin_ && admin_->ready()) {
                    admin_->paste(target, this, [this, id = record.id, generation](auto result) {
                        if (generation != pasteGeneration_)
                            return;
                        if (!result)
                            error(result.error());
                        else
                            clipboard_->touch(id);
                    });
                } else if (auto result = DesktopServices::sendPaste(target); !result)
                    error(result.error());
                else
                    clipboard_->touch(record.id);
            });
    timer->start();
}
void Application::lockScreen() {
    if (settingsBusy_) return;
    if (preview_ || updateInstalling_ || !settings_.lockEnabled || settings_.lockPaused || screenLock_->active())
        return;
    // A Shell request already handed to another process cannot be recalled
    // atomically. Wait for its bounded result before promising a locked desktop.
    if (shellJobs_.hasActiveActions() || (admin_ && admin_->busy()) ||
        (gestures_ && gestures_->hasActiveActions())) {
        error(tx(u"正在完成启动或导入请求，请稍后锁定"));
        return;
    }
    if (auto result = desktop_.capturePanelNavigation(false); !result) {
        error(result.error());
        return;
    }
    if (auto result = screenLock_->start(settings_.requirePassword, settings_.protectedPassword,
                                         settings_.lockHotkey); !result)
        error(result.error());
}
void Application::buildTrayMenu() {
    menu_ = std::make_unique<QMenu>();
    menu_->addAction(tx(u"启动面板"), this, &Application::showLauncher);
    menu_->addAction(tx(u"剪贴板"), this, &Application::showClipboard);
    auto* lock = menu_->addAction(tx(u"立即黑屏"), this, &Application::lockScreen);
    auto* pauseLock = menu_->addAction(tx(u"暂停黑屏遮挡"));
    pauseLock->setCheckable(true);
    connect(pauseLock, &QAction::triggered, this, [this](bool checked) {
        auto next = settings_;
        next.lockPaused = checked;
        applySettings(next);
    });
    connect(menu_.get(), &QMenu::aboutToShow, this, [this, lock, pauseLock] {
        lock->setText(settings_.requirePassword ? tx(u"立即黑屏（密码 + 回车解锁）")
                                               : tx(u"立即黑屏（回车解锁）"));
        lock->setEnabled(!preview_ && settings_.lockEnabled && !settings_.lockPaused &&
                         !screenLock_->active());
        pauseLock->setEnabled(settings_.lockEnabled && !screenLock_->active());
        pauseLock->setChecked(settings_.lockPaused);
    });
    menu_->addSeparator();
    auto* pause = menu_->addAction(tx(u"暂停剪贴板记录"));
    pause->setCheckable(true);
    pause->setChecked(settings_.clipboardPaused);
    connect(pause, &QAction::triggered, this, [this, pause](bool checked) {
        auto next = settings_;
        next.clipboardPaused = checked;
        applySettings(next, [this, action = QPointer<QAction>(pause)](auto result) {
            if (!result) {
                if (action) action->setChecked(settings_.clipboardPaused);
                error(result.error());
            }
        });
    });
    menu_->addAction(tx(u"设置"), this, &Application::showSettings);
    menu_->addAction(tx(u"切换黑白主题"), this, [this] {
        auto next = settings_;
        next.theme = effectiveTheme() == QStringLiteral("dark") ? QStringLiteral("light")
                                                                : QStringLiteral("dark");
        applySettings(next);
    });
    menu_->addSeparator();
    menu_->addAction(tx(u"关于"), this, [this] {
        QMessageBox::information(
            nullptr, QStringLiteral("MT"),
            tx(u"MT %1 · 原生开发预览\nC++23 · Qt "
                           "Widgets\n启动器、剪贴板、鼠标手势和黑屏遮挡已接入。\n数据目录：%2")
                .arg(QString::fromLatin1(TOOLS_VERSION), store_.root()));
    });
    menu_->addAction(tx(u"退出"), qApp, &QCoreApplication::quit);
}
QString Application::effectiveTheme() const {
    if (settings_.theme != QStringLiteral("auto"))
        return settings_.theme;
    return qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light ? QStringLiteral("light")
                                                                       : QStringLiteral("dark");
}
void Application::applyClipboardBehavior() {
    if (!clipboardPanel_)
        return;
    ClipboardBehavior behavior;
    behavior.hoverPreview = settings_.hoverPreview;
    behavior.spacePreview = settings_.spacePreview;
    behavior.previewLeft = settings_.previewSide != QStringLiteral("right");
    behavior.openKey = settings_.openKey.isEmpty() ? 0 : Qt::Key_A + (settings_.openKey[0].unicode() - u'A');
    behavior.openButton = settings_.openMouseButton == QStringLiteral("right")    ? Qt::RightButton
                          : settings_.openMouseButton == QStringLiteral("middle") ? Qt::MiddleButton
                                                                                   : Qt::NoButton;
    clipboardPanel_->setBehavior(behavior);
}
void Application::openRecord(const ClipboardRecord& record) {
    if (screenLock_ && screenLock_->active())
        return;
    if (settings_.maskSensitive && record.sensitive)
        return;
    if (preview_) {
        tray_.showMessage(tx(u"隔离预览"), tx(u"预览模式未启动程序或打开链接。"));
        return;
    }
    // Files and paths are revealed in Explorer rather than executed.
    if (record.kind == ClipKind::Files) {
        if (!DesktopServices::revealInExplorer(record.files.value(0)))
            error(tx(u"文件已不存在或无法定位"));
        return;
    }
    const auto target = record.kind == ClipKind::Image ? OpenTarget{} : openTarget(record.text);
    switch (target.kind) {
    case OpenKind::Url:
    case OpenKind::Email: {
        LaunchItem item;
        item.target = target.kind == OpenKind::Email ? QStringLiteral("mailto:") + target.value
                                                     : target.value;
        launch(item);
        return;
    }
    case OpenKind::Path:
        if (!DesktopServices::revealInExplorer(target.value))
            error(tx(u"文件已不存在或无法定位"));
        return;
    case OpenKind::None:
        break;
    }
    error(tx(u"此内容不是可打开的链接、邮箱或本地路径"));
}
void Application::clearHistory() {
    clipboard_->clear([this](bool ok) {
        if (ok)
            tray_.showMessage(QStringLiteral("MT"), tx(u"剪贴板历史已清空"));
    });
}
void Application::importHopy(const QString& folder) {
    clipboard_->importHopy(folder, [this](int imported, int skipped) {
        if (imported < 0)
            return; // the service already reported why; the history is unchanged
        auto message = tx(u"已从 Hopy 导入 %1 条记录").arg(imported);
        if (skipped)
            message += tx(u"，%1 条因附件缺失或超限被跳过").arg(skipped);
        tray_.showMessage(QStringLiteral("MT"), message);
        if (settingsDialog_)
            QMessageBox::information(settingsDialog_, tx(u"导入完成"), message);
    });
}
void Application::importHLaunch(const QString& folder) {
    launcher_->importHLaunch(folder, [this](auto result) {
        if (!result) return; // service reports failures
        auto message = tx(u"已从 HLaunch 导入 %1 个启动项").arg((*result)[0]);
        if ((*result)[1]) message += tx(u"，%1 个与现有项目完全相同已跳过").arg((*result)[1]);
        if ((*result)[2]) message += tx(u"，%1 个因路径或数据无效被跳过").arg((*result)[2]);
        tray_.showMessage(QStringLiteral("MT"), message);
        if (settingsDialog_) QMessageBox::information(settingsDialog_, tx(u"导入完成"), message);
    });
}

void Application::applyEdgeActivation() {
    EdgeOptions options;
    // Preview mode never samples the real desktop.
    options.enabled = !preview_ && !(screenLock_ && screenLock_->active()) &&
                      settings_.launcherEnabled && settings_.launcherEdgeEnabled;
    for (const auto& key : settings_.launcherEdgeZones)
        if (const auto zone = edgeZoneFromKey(key); zone != EdgeZone::None)
            options.config.zones.append(zone);
    options.config.outerOnly = settings_.launcherEdgeMode != QStringLiteral("every");
    options.suppressFullscreen = settings_.launcherEdgeSuppressFullscreen;
    options.blockList = settings_.launcherEdgeBlocklist;
    options.allowList = settings_.launcherEdgeAllowlist;
    edge_.configure(options);
}
void Application::showLauncherAtEdge(EdgeZone zone) {
    if (gestures_) gestures_->blockNow();
    if (screenLock_ && screenLock_->active())
        return;
    if (!settings_.launcherEnabled)
        return;
    auto& panel = launcherPanel();
    // Edge activation only shows; it never hides an open panel.
    if (panel.isVisible()) {
        panel.raise();
        return;
    }
    const auto cursor = QCursor::pos();
    auto* screen = QGuiApplication::screenAt(cursor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    panel.fitToWorkArea(area);
    const auto size = panel.size();
    auto x = std::clamp(cursor.x() - size.width() / 2, area.left(),
                        std::max(area.left(), area.right() - size.width() + 1));
    auto y = std::clamp(cursor.y() - size.height() / 2, area.top(),
                        std::max(area.top(), area.bottom() - size.height() + 1));
    const auto key = edgeZoneKey(zone);
    if (key.contains(QStringLiteral("left"), Qt::CaseInsensitive))
        x = area.left();
    if (key.contains(QStringLiteral("right"), Qt::CaseInsensitive))
        x = area.right() - size.width() + 1;
    if (key.startsWith(QStringLiteral("top")))
        y = area.top();
    if (key.startsWith(QStringLiteral("bottom")))
        y = area.bottom() - size.height() + 1;
    const auto target = DesktopServices::foreground();
    if (target.handle)
        lastTarget_ = target;
    if (clipboardPanel_)
        clipboardPanel_->hide();
    panel.showNear({x, y});
}
void Application::seedPreview() {
    if (launcher_->catalog().items.isEmpty()) {
        const QStringList names{QStringLiteral("Notepad3"),
                                QStringLiteral("Notepad4"),
                                QStringLiteral("Notepad--"),
                                QStringLiteral("skylark"),
                                QStringLiteral("Sublime"),
                                QString{},
                                QStringLiteral("Dadroit"),
                                QStringLiteral("BCompare"),
                                QString{},
                                QStringLiteral("dbx"),
                                QStringLiteral("XTerminal"),
                                QStringLiteral("OSS2"),
                                QStringLiteral("Wireshark"),
                                QStringLiteral("Reqable"),
                                QStringLiteral("studio"),
                                QString{},
                                QStringLiteral("RisohEditor"),
                                QStringLiteral("DBG_ypp"),
                                QStringLiteral("A_demo"),
                                QStringLiteral("apimonitor"),
                                QStringLiteral("scomm"),
                                QStringLiteral("sscom5.13.1"),
                                QStringLiteral("mbclient"),
                                QStringLiteral("xTools"),
                                QStringLiteral("procexp64"),
                                QStringLiteral("SystemInform"),
                                QStringLiteral("TMX64"),
                                QStringLiteral("Procmon64")};
        const QStringList icons{QStringLiteral("notepad.exe"), QStringLiteral("cmd.exe"),
                                QStringLiteral("control.exe"), QStringLiteral("taskmgr.exe")};
        const auto system = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
        const auto category = launcher_->catalog().selectedCategory;
        for (int i = 0; i < names.size(); ++i)
            if (!names[i].isEmpty()) {
                LaunchItem item;
                item.name = names[i];
                item.target =
                    QDir(system).filePath(QStringLiteral("System32/") + icons[i % icons.size()]);
                launcher_->put(category, i, item);
            }
    }
    // Synthetic records covering every kind; images are drawn here, file paths are
    // fictitious and never opened in preview mode.
    ClipboardRecords seeds;
    const auto text = [&](const QString& value) {
        ClipboardRecord record;
        record.text = value;
        seeds.append(record);
        return &seeds.last();
    };
    const auto image = [&](QImage value) {
        ClipboardRecord record;
        record.kind = ClipKind::Image;
        record.pendingImage = std::move(value);
        seeds.append(record);
        return &seeds.last();
    };
    const auto files = [&](const QStringList& paths) {
        ClipboardRecord record;
        record.kind = ClipKind::Files;
        record.files = paths;
        record.text = paths.join(u'\n');
        seeds.append(record);
        return &seeds.last();
    };
    text(QStringLiteral("cmake --build --preset win-static-release"))->pinned = true;
    image(previewImage(QSize(640, 400), 0))->favorite = true;
    text(QStringLiteral("让常用的工具，回到顺手的位置。\n更少的打扰，更专注的桌面。"))->favorite = true;
    files({QStringLiteral("D:/Projects/tools/docs/设计说明.md")});
    {
        ClipboardRecord record;
        record.kind = ClipKind::Html;
        record.html = QStringLiteral("<h3>发布检查清单</h3><ul><li><b>静态</b> Qt 构建</li>"
                                     "<li>深浅主题截图</li><li>单 EXE 导入表</li></ul>");
        record.text = QStringLiteral("发布检查清单\n静态 Qt 构建\n深浅主题截图\n单 EXE 导入表");
        seeds.append(record);
    }
    text(QStringLiteral("https://example.com/design/desktop"));
    image(previewImage(QSize(360, 900), 1));
    files({QStringLiteral("D:/Projects/tools/resources/icons/lucide/search.svg"),
           QStringLiteral("D:/Projects/tools/resources/icons/lucide/star.svg"),
           QStringLiteral("D:/Projects/tools/resources/icons/lucide/settings.svg")});
    text(QStringLiteral(
        "#include <QApplication>\n\nint main(int argc, char* argv[]) {\n    QApplication app(argc, "
        "argv);\n    return app.exec();\n}"));
    text(QStringLiteral("#4DAAFC"));
    text(QStringLiteral("示例密码 mt-Preview-2026"))->sensitive = true;
    {
        QStringList paragraphs;
        for (int i = 1; i <= 12; ++i)
            paragraphs << QStringLiteral("第 %1 段：长文本用于验证预览窗口的滚动和右上角百分比。"
                                         "内容在面板高度内显示，超出部分可以 Ctrl+滚轮查看。")
                              .arg(i);
        text(paragraphs.join(QStringLiteral("\n\n")));
    }
    image(previewImage(QSize(1280, 720), 2));
    files({QStringLiteral("C:/Users/Public/Documents/会议纪要 2026-10-28.docx"),
           QStringLiteral("C:/Users/Public/Pictures/截图 01.png")});
    text(QStringLiteral("hello@example.com"));
    text(QStringLiteral("下一次迭代\n统一两个面板的快捷操作…"));
    text(QStringLiteral("仅用于界面验证的示例数据"));
    // The store keeps at most a few captures in flight; space them out.
    for (int i = static_cast<int>(seeds.size()) - 1, step = 0; i >= 0; --i, ++step) {
        auto record = seeds[i];
        record.captured = QDateTime::currentDateTimeUtc().addSecs(-i * 150);
        QTimer::singleShot(step * 80, this, [this, record] { clipboard_->capture(record); });
    }
}
void Application::saveEvidence(const QString& folder) {
    QDir().mkpath(folder);
    auto& launch = launcherPanel();
    auto& clip = clipboardPanel();
    launch.keepOpen = true;
    launch.showNear(QPoint(160, 180));
    clip.showNear(QPoint(510, 180), false);
    // Open the hover/Space preview for the second record; the full record arrives
    // asynchronously from the storage thread, so capture after it is presented.
    clip.handleKey(Qt::Key_Home);
    clip.handleKey(Qt::Key_Down);
    clip.handleKey(Qt::Key_Space);
    QTimer::singleShot(600, this, [this, folder] { saveEvidenceRenders(folder); });
}
void Application::saveEvidenceRenders(const QString& folder) {
    auto& launch = launcherPanel();
    auto& clip = clipboardPanel();
    QJsonObject report{{QStringLiteral("version"), QString::fromLatin1(TOOLS_VERSION)},
                       {QStringLiteral("preview"), preview_},
                       {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
                       {QStringLiteral("launcherWidth"), launch.width()},
                       {QStringLiteral("launcherHeight"), launch.height()},
                       {QStringLiteral("clipboardWidth"), clip.width()},
                       {QStringLiteral("clipboardHeight"), clip.height()},
                       {QStringLiteral("uiFontFamily"), QFontInfo(qApp->font()).family()},
                       {QStringLiteral("uiFontPointSize"), qApp->font().pointSizeF()},
                       {QStringLiteral("launcherOpacity"), launch.windowOpacity()},
                       {QStringLiteral("clipboardOpacity"), clip.windowOpacity()},
                       {QStringLiteral("errors"), QJsonArray::fromStringList(errors_)}};
    for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
        theme_.apply(mode);
        const auto capture = [&](QWidget& panel, const QString& name) {
            // Contents only: corners and shadow belong to DWM and need a screen capture.
            QPixmap image(panel.size() * panel.devicePixelRatioF());
            image.setDevicePixelRatio(panel.devicePixelRatioF());
            image.fill(Qt::transparent);
            panel.render(&image);
            image.save(QDir(folder).filePath(name + u'-' + mode + QStringLiteral(".png")));
        };
        capture(launch, QStringLiteral("launcher"));
        capture(clip, QStringLiteral("clipboard"));
        if (auto* preview = clip.findChild<Panel*>(); preview && preview->isVisible())
            capture(*preview, QStringLiteral("clipboard-preview"));
        // Toolbar with one filter checked, to review the selected-filter style.
        if (auto* toolbar = clip.findChild<QWidget*>(QStringLiteral("Toolbar"))) {
            clip.handleKey(Qt::Key_Right);
            QPixmap strip(toolbar->size() * toolbar->devicePixelRatioF());
            strip.setDevicePixelRatio(toolbar->devicePixelRatioF());
            strip.fill(theme_.color(QStringLiteral("chrome")));
            toolbar->render(&strip);
            strip.save(QDir(folder).filePath(QStringLiteral("clipboard-filter-") + mode +
                                             QStringLiteral(".png")));
            clip.handleKey(Qt::Key_Left);
        }
        menu_->ensurePolished();
        menu_->adjustSize();
        capture(*menu_, QStringLiteral("menu"));
        {
            SettingsDialog dialog(theme_, settings_, store_.root(),
                                  [](const Settings&, SettingsDialog::Completion done) { done({}); });
            if (auto* pages = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsPages")))
                pages->setCurrentIndex(3); // clipboard page
            dialog.resize(1000, 760);
            dialog.ensurePolished();
            QPixmap page(dialog.size() * dialog.devicePixelRatioF());
            page.setDevicePixelRatio(dialog.devicePixelRatioF());
            dialog.render(&page);
            page.save(QDir(folder).filePath(QStringLiteral("settings-clipboard-") + mode +
                                            QStringLiteral(".png")));
        }
    }
    theme_.apply(settings_.theme);
    QFile file(QDir(folder).filePath(QStringLiteral("native-smoke.json")));
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(report).toJson());
}
} // namespace mt
