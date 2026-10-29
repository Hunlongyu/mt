#pragma once
#include "DesktopServices.h"
#include "modules/gestures/GestureRules.h"
#include <string>
namespace mt {
// Per-controller state, never a global window history. A window property binds
// the saved identity to this window's lifetime, including same-process HWND reuse.
class GestureActions final {
  public:
    GestureActions();
    ~GestureActions();
    GestureActions(const GestureActions&) = delete;
    GestureActions& operator=(const GestureActions&) = delete;
    bool available(const GestureAction& action, WindowTarget target) const;
    std::expected<void, QString> execute(const GestureAction& action, WindowTarget target,
                                        quintptr foregroundAtStart);
  private:
    bool savedWindowValid() const;
    void forget();
    WindowTarget lastMinimized_;
    std::wstring marker_;
};
std::expected<QString, QString> gestureProgramDirectory(WindowTarget target);
}
