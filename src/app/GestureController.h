#pragma once
#include "infrastructure/settings/Settings.h"
#include "modules/gestures/GestureRules.h"
#include "platform/windows/GestureInput.h"
#include "platform/windows/GestureActions.h"
#include <functional>
#include <memory>
#include <QPointer>
#include <vector>
class QWidget;
namespace mt {
class GestureOverlay;
class ShellJobs;
class Theme;
class AdminBridge;
class GestureController final : public QObject {
    Q_OBJECT
  public:
    GestureController(Theme& theme, QString root, bool preview,
                      std::function<bool()> blocked, QObject* parent = nullptr);
    ~GestureController() override;
    std::expected<void, QString> configure(const Settings& settings);
    void blockNow();
    bool hasActiveActions() const;
    void setLauncherWindow(QWidget* window);
    void setAdminBridge(AdminBridge* bridge) {
        admin_ = bridge;
    }
  Q_SIGNALS:
    void failed(const QString& message);
    void recognized(const QString& directions, const QString& action);
  private:
    bool eventFilter(QObject*, QEvent*) override;
    void updateLauncherWindow();
    void receive(const GestureFrame& frame);
    void hide();
    Theme& theme_;
    QString root_;
    bool preview_;
    std::function<bool()> blocked_;
    Settings settings_;
    GestureRules rules_;
    GestureInput input_;
    QPointer<QWidget> launcherWindow_;
    GestureActions actions_;
    AdminBridge* admin_ = nullptr;
    std::unique_ptr<ShellJobs> jobs_;
    std::vector<std::unique_ptr<GestureOverlay>> overlays_;
    quint64 displayedStroke_ = 0;
};
}
