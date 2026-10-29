#pragma once
#include <QObject>
#include <memory>

class QMenu;

namespace mt {
class Theme;
class MenuFrame;
// Applies the same surface to application menus and standard Qt editor menus.
class PopupMenuStyle final : public QObject {
  public:
    explicit PopupMenuStyle(Theme& theme);
    ~PopupMenuStyle() override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void applyFrame(QMenu& menu);
    Theme& theme_;
    std::unique_ptr<MenuFrame> frame_;
};
} // namespace mt
