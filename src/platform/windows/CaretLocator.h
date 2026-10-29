#pragma once
#include <QRect>
#include <QString>
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace mt {
// Finds the text caret of the foreground window in physical screen pixels.
// Classic Win32 controls report it directly; Chromium/Electron (browsers,
// VS Code) and WPF only expose it through accessibility, so UI Automation and
// MSAA are consulted too. Every candidate is checked for plausibility, because
// many apps park a placeholder caret at their client origin.
// Requests originate on the GUI thread; COM, UIA and all native probes run on
// one windowless MTA thread. Only the newest request is delivered.
class CaretLocator final : public QObject {
    Q_OBJECT
  public:
    CaretLocator();
    ~CaretLocator();
    CaretLocator(const CaretLocator&) = delete;
    CaretLocator& operator=(const CaretLocator&) = delete;
    using Completion = std::function<void(std::optional<QRect>, QString)>;
    void request(quintptr foregroundWindow, QObject* context, Completion done);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<quint64> generation_{0};
};
} // namespace mt
