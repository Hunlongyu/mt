#pragma once
#include <QObject>
#include <memory>

class QWidget;

namespace mt {
class Theme;
class MenuFrame;
// Applies the same frame to menus, editor menus and settings dropdowns.
class PopupMenuStyle final : public QObject {
  public:
    explicit PopupMenuStyle(Theme& theme);
    ~PopupMenuStyle() override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void applyFrame(QWidget& popup);
    Theme& theme_;
    std::unique_ptr<MenuFrame> frame_;
};
} // namespace mt
