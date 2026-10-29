#pragma once
#include "LockInput.h"
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#include <vector>

namespace mt {
class LockSession;
class BlackoutWindow;
class ScreenLockService final : public QObject {
    Q_OBJECT
  public:
    explicit ScreenLockService(QObject* parent = nullptr);
    ~ScreenLockService() override;
    bool active() const {
        return active_;
    }
    bool locked() const {
        return locked_;
    }
    std::expected<void, QString> start(bool required, const QString& protectedPassword,
                                       const QString& shortcut);
    void stop();
    // Used at host shutdown, before QApplication and platform services die.
    void shutdown();
  Q_SIGNALS:
    void activeChanged(bool active);
    void failed(const QString& message);
    void unlocked();

  private:
    void rebuildCovers();
    void finish();
    void press(int key, Qt::KeyboardModifiers modifiers, QChar character, bool repeat = false);
    LockInput input_;
    std::unique_ptr<LockSession> session_;
    std::vector<std::unique_ptr<BlackoutWindow>> covers_;
    QTimer pulse_;
    QElapsedTimer starting_;
    bool active_ = false;
    bool locked_ = false;
    bool stopping_ = false;
    quint64 generation_ = 0;
};
} // namespace mt
