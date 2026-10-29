#pragma once
#include <QThread>
#include <QList>
#include <atomic>
#include <memory>

namespace mt {
// Dedicated input thread. No QWidget or password data is used by its hook.
// The GUI pulses its lease; expiry releases input and asks Windows to hide covers.
class LockSession final : public QThread {
    Q_OBJECT
  public:
    explicit LockSession(QObject* parent = nullptr);
    ~LockSession() override;
    void pulse();
    void setCoverWindows(const QList<quintptr>& windows);
    void stop();
  Q_SIGNALS:
    void ready();
    void keyPressed(int key, int modifiers, ushort character);
    void failed(const QString& message);
    void emergencyExit();
    void inputReset();
  protected:
    void run() override;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mt
