#pragma once
#include "infrastructure/settings/Settings.h"
#include <QWidget>
#include <memory>
namespace mt {
class Theme;
class GestureSettings final : public QWidget {
    Q_OBJECT
  public:
    GestureSettings(Theme& theme, const Settings& settings, QWidget* parent = nullptr);
    ~GestureSettings() override;
    std::expected<void, QString> write(Settings& settings) const;
  Q_SIGNALS:
    void changed();

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace mt
