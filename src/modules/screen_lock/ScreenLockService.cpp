#include "ScreenLockService.h"
#include "infrastructure/i18n/I18n.h"
#include "platform/windows/LockPassword.h"
#include "platform/windows/LockSession.h"
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QSessionManager>
#include <QWidget>
#include <QWindow>
#include <functional>

namespace mt {
class BlackoutWindow final : public QWidget {
  public:
    std::function<void(QKeyEvent*)> fallback;
    BlackoutWindow()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setObjectName(QStringLiteral("mtBlackout"));
        setWindowTitle(QStringLiteral("MT · Blackout"));
        setStyleSheet(QStringLiteral("background-color: #000000; border: none;"));
        setCursor(Qt::BlankCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_QuitOnClose, false);
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
    }
    void keyPressEvent(QKeyEvent* event) override {
        // WH_KEYBOARD_LL swallows normal input. If Windows removed it, focused
        // window input remains a second route through the same password policy.
        if (fallback)
            fallback(event);
        event->accept();
    }
};
ScreenLockService::ScreenLockService(QObject* parent) : QObject(parent) {
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this](QSessionManager&) {
        shutdown(); // release before Qt asks top-level windows to close at logoff
    }, Qt::DirectConnection);
    pulse_.setInterval(250);
    connect(&pulse_, &QTimer::timeout, this, [this] {
        if (active_ && !locked_ && !stopping_ && starting_.elapsed() > 2500) {
            stop();
            Q_EMIT failed(tx(u"黑屏启动失败，输入或电源保护未就绪"));
            if (session_ && !session_->isRunning())
                finish();
            return;
        }
        if (session_)
            session_->pulse();
        if (locked_)
            for (const auto& cover : covers_)
                cover->raise();
    });
    const auto watch = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [this] { rebuildCovers(); });
    };
    for (auto* screen : QGuiApplication::screens())
        watch(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, watch](QScreen* screen) {
        watch(screen);
        rebuildCovers();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { rebuildCovers(); });
}
ScreenLockService::~ScreenLockService() {
    shutdown();
}
std::expected<void, QString> ScreenLockService::start(bool required, const QString& password,
                                                      const QString& shortcut) {
    if (active_)
        return {};
    QString plain;
    if (required) {
        auto decoded = revealLockPassword(password);
        if (!decoded)
            return std::unexpected(decoded.error());
        plain = std::move(*decoded);
    }
    const auto configured = input_.begin(
        required, plain, QKeySequence::fromString(shortcut, QKeySequence::PortableText));
    plain.fill(QChar{});
    plain.clear();
    if (!configured)
        return configured;
    if (QGuiApplication::screens().isEmpty())
        return std::unexpected(tx(u"没有可用于黑屏遮挡的显示器"));
    active_ = true;
    stopping_ = false;
    const auto generation = ++generation_;
    Q_EMIT activeChanged(true); // gate host actions before acquiring input
    session_ = std::make_unique<LockSession>();
    connect(session_.get(), &LockSession::ready, this, [this, generation] {
        if (generation != generation_ || !active_ || stopping_ || !session_->isRunning())
            return;
        locked_ = true;
        rebuildCovers();
        if (!covers_.empty())
            covers_.front()->activateWindow();
    });
    connect(session_.get(), &LockSession::keyPressed, this,
            [this, generation](int key, int mods, ushort ch) {
                if (generation == generation_)
                    press(key, Qt::KeyboardModifiers(mods), QChar(ch));
            });
    connect(session_.get(), &LockSession::failed, this, [this, generation](const QString& message) {
        if (generation == generation_)
            Q_EMIT failed(message);
    });
    connect(session_.get(), &LockSession::inputReset, this, [this, generation] {
        if (generation == generation_)
            input_.clear();
    });
    connect(session_.get(), &LockSession::finished, this, [this, generation] {
        if (generation == generation_)
            finish();
    });
    rebuildCovers(); // hidden, native windows exist before the input lease starts
    session_->pulse();
    starting_.start();
    pulse_.start();
    session_->start();
    return {};
}
void ScreenLockService::rebuildCovers() {
    if (!active_ || stopping_ || !session_)
        return;
    if (QGuiApplication::screens().isEmpty()) {
        stop();
        Q_EMIT failed(tx(u"显示器已断开，已解除黑屏遮挡"));
        return;
    }
    std::vector<std::unique_ptr<BlackoutWindow>> next;
    QList<quintptr> handles;
    for (auto* screen : QGuiApplication::screens()) {
        auto cover = std::make_unique<BlackoutWindow>();
        cover->winId();
        cover->windowHandle()->setScreen(screen);
        cover->setGeometry(screen->geometry());
        cover->fallback = [this](QKeyEvent* event) {
            press(event->key(), event->modifiers(),
                  event->text().size() == 1 ? event->text()[0] : QChar{}, event->isAutoRepeat());
        };
        if (locked_)
            cover->show();
        handles.append(quintptr(cover->winId()));
        next.push_back(std::move(cover));
    }
    session_->setCoverWindows(handles);
    covers_ = std::move(next);
}
void ScreenLockService::press(int key, Qt::KeyboardModifiers mods, QChar ch, bool repeat) {
    if (!locked_ || stopping_)
        return;
    switch (input_.press(key, mods, ch, repeat)) {
    case LockInput::Result::Unlock:
        stop();
        break;
    case LockInput::Result::Rejected:
        QApplication::beep();
        break;
    case LockInput::Result::Ignored:
        break;
    }
}
void ScreenLockService::stop() {
    if (!active_)
        return;
    stopping_ = true;
    if (session_)
        session_->stop();
}
void ScreenLockService::finish() {
    ++generation_;
    // QThread::finished can precede TLS destruction; join before dropping ownership.
    if (session_)
        session_->wait();
    pulse_.stop();
    covers_.clear();
    session_.reset();
    input_.clear();
    const bool wasActive = active_;
    active_ = locked_ = stopping_ = false;
    if (wasActive) {
        Q_EMIT activeChanged(false);
        Q_EMIT unlocked();
    }
}
void ScreenLockService::shutdown() {
    if (session_) {
        session_->disconnect(this);
        session_->stop();
    }
    finish();
}
} // namespace mt
