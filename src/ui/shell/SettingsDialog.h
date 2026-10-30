#pragma once
#include "infrastructure/settings/Settings.h"
#include <QDialog>
#include <functional>
#include <memory>
namespace mt {
class Theme;
class SettingsDialog final : public QDialog {
    Q_OBJECT
  public:
    using Completion = std::function<void(std::expected<void, QString>)>;
    using Apply = std::function<void(const Settings&, Completion)>;
    SettingsDialog(Theme& theme, const Settings& settings, const QString& dataRoot, Apply apply,
                   QWidget* parent = nullptr);
    ~SettingsDialog() override;
    void reject() override;
    bool hasUnsavedChanges() const;
    void showUpdatePage();
    void setSystemIntegrationStatus(bool preview, const QString& message);
    void setUpdateStatus(const QString& message, bool canCheck, bool canUpdate,
                         bool ready, bool canCancel);

  protected:
    void closeEvent(QCloseEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
    void save(bool closeAfter = false);
    void discard();
  Q_SIGNALS:
    void panelOpacityPreview(int launcher, int clipboard);
    void clearHistoryRequested();
    void importHopyRequested(const QString& folder);
    void importHLaunchRequested(const QString& folder);
    void checkUpdatesRequested();
    void updateRequested();
    void cancelUpdateRequested();
};
} // namespace mt
