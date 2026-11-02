#pragma once
#include <QObject>
#include <memory>

class QWindow;

namespace mt {
// Scoped to an internal drag. Windows OLE consumes wheel messages before Qt's
// widget wheel handlers; observe the GUI thread's queue while that loop runs.
class DragWheelRouter final : public QObject {
    Q_OBJECT
  public:
    explicit DragWheelRouter(QObject* parent = nullptr);
    ~DragWheelRouter() override;
    bool start(QWindow& window);
    void stop();
    // Apply any wheel message already removed from the queue before a drop.
    void flush();
  Q_SIGNALS:
    void stepped(int steps);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mt
