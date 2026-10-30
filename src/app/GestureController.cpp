#include "GestureController.h"
#include "infrastructure/i18n/I18n.h"
#include "modules/gestures/GestureOverlay.h"
#include "modules/gestures/GestureRecognizer.h"
#include "platform/windows/AdminBridge.h"
#include "platform/windows/GestureActions.h"
#include "platform/windows/ShellJobs.h"
#include "ui/components/Theme.h"
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QWidget>

namespace mt {
GestureController::GestureController(Theme& theme, QString root, bool preview,
    std::function<bool()> blocked, QObject* parent)
    : QObject(parent), theme_(theme), root_(std::move(root)), preview_(preview), blocked_(std::move(blocked)) {
    connect(&input_, &GestureInput::frame, this, &GestureController::receive);
    connect(&input_, &GestureInput::failed, this, &GestureController::failed);
    connect(&input_, &GestureInput::clickReplayRequested, this,
            [this](WindowTarget target, quintptr foreground, QPoint point, const QString& button,
                   quint64 generation, quint64 cancellation) {
                if (admin_ && admin_->ready() && settings_.adminSupport && !blocked_() &&
                    generation == input_.generation() && cancellation == input_.cancellation())
                    admin_->replayClick(target, foreground, point, button);
            });
    auto* timer = new QTimer(this);
    timer->setInterval(25);
    connect(timer, &QTimer::timeout, this, [this] {
        const bool blocked = blocked_() || (admin_ && admin_->busy());
        input_.setAdminAvailable(settings_.adminSupport && admin_ && admin_->ready());
        input_.setBlocked(blocked);
        if (blocked)
            hide();
    });
    if (!preview_) timer->start();
    const auto screensChanged = [this] { blockNow(); overlays_.clear(); };
    connect(qApp, &QGuiApplication::screenAdded, this, screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, screensChanged);
}
GestureController::~GestureController() {
    input_.setBlocked(true);
    if (jobs_) jobs_->shutdown();
    hide();
}
void GestureController::setLauncherWindow(QWidget* window) {
    if (launcherWindow_ == window) return;
    if (launcherWindow_) launcherWindow_->removeEventFilter(this);
    input_.setLauncherWindow(0);
    launcherWindow_ = window;
    if (window) window->installEventFilter(this);
    updateLauncherWindow();
}
void GestureController::updateLauncherWindow() {
    input_.setLauncherWindow(launcherWindow_ && launcherWindow_->isVisible()
                                 ? quintptr(launcherWindow_->internalWinId()) : 0);
}
bool GestureController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == launcherWindow_) {
        switch (event->type()) {
        case QEvent::Show:
            input_.setLauncherWindow(quintptr(launcherWindow_->internalWinId()));
            break;
        case QEvent::WinIdChange:
            updateLauncherWindow();
            break;
        case QEvent::Hide:
        case QEvent::Destroy:
            input_.setLauncherWindow(0);
            hide();
            break;
        default: break;
        }
    }
    return QObject::eventFilter(watched, event);
}
void GestureController::hide() {
    for (const auto& overlay : overlays_) overlay->dismiss();
    displayedStroke_ = 0;
}
void GestureController::blockNow() {
    input_.setBlocked(true);
    if (jobs_) jobs_->cancelQueuedActions();
    hide();
}
bool GestureController::hasActiveActions() const {
    return (jobs_ && jobs_->hasActiveActions()) || (admin_ && admin_->busy());
}
std::expected<void, QString> GestureController::configure(const Settings& next) {
    auto parsed = GestureRules::parse(next.gestureActions, next.gestureApplicationActions,
                                      next.excludedProcesses, next.gestureAllowlist);
    if (!parsed) return std::unexpected(parsed.error());
    GestureInputOptions options;
    options.enabled = next.gesturesEnabled && !preview_;
    options.trigger = next.gestureTrigger;
    options.timeout = next.gestureTimeout;
    options.suppressFullscreen = next.disableGesturesFullscreen;
    options.rules = *parsed;
    if (auto result = input_.configure(options); !result) return result;
    hide();
    jobs_.reset(); // discard queued launches from the previous configuration
    jobs_ = std::make_unique<ShellJobs>();
    rules_ = *parsed;
    settings_ = next;
    input_.setBlocked(blocked_());
    return {};
}
void GestureController::receive(const GestureFrame& frame) {
    const bool latestStroke = frame.stroke == input_.stroke();
    if (!latestStroke && !frame.finished) return;
    if (frame.generation != input_.generation() || frame.cancellation != input_.cancellation() ||
        !settings_.gesturesEnabled || preview_ || blocked_()) {
        if (latestStroke) hide();
        return;
    }
    if (frame.cancelled || !frame.drawing || !GestureInput::isCurrent(frame)) {
        if (latestStroke) hide();
        return;
    }
    GestureRecognizer recognizer(settings_.gestureThreshold);
    for (const auto point : frame.points) recognizer.append(point);
    const auto actions = rules_.forProcess(frame.process);
    const auto recognition = recognizer.match(actions.keys());
    const auto match = actions.constFind(recognition.key);
    // The restricted helper deliberately rejects its parent process. Launcher
    // actions belong to the GUI process even when administrator support is on.
    const bool useAdmin = settings_.adminSupport && admin_ && admin_->ready() &&
                         frame.target.processId != quint32(QCoreApplication::applicationPid());
    const bool actionable =
        match != actions.end() && (useAdmin ||
                                   actions_.available(*match, frame.target));
    // A retained completion may still execute, but must never repaint its old
    // path after another physical press has started a new stroke.
    if (latestStroke && (settings_.gestureTrail || settings_.gestureShowName)) {
        // The bounded mailbox may skip the previous release or this stroke's
        // initial stationary frame when the GUI is busy. Never reuse its pixels.
        if (displayedStroke_ != frame.stroke) hide();
        displayedStroke_ = frame.stroke;
        QPolygonF points;
        for (const auto point : frame.points) points.append(GestureInput::logicalPoint(point));
        QString label;
        if (settings_.gestureShowName && !recognition.directions.isEmpty())
            label = actionable ? match->label() : tx(u"手势无动作");
        const auto screens = QGuiApplication::screens();
        if (overlays_.size() != size_t(screens.size())) {
            overlays_.clear();
            for (qsizetype i = 0; i < screens.size(); ++i)
                overlays_.push_back(std::make_unique<GestureOverlay>(theme_));
        }
        for (qsizetype i = 0; i < screens.size(); ++i) {
            overlays_[size_t(i)]->setGeometry(screens[i]->geometry());
            overlays_[size_t(i)]->present(points, label, settings_.gestureTrail, actionable);
        }
    }
    if (!frame.finished) return;
    if (latestStroke) {
        if (DesktopServices::clientAnimationsEnabled()) {
            for (const auto& overlay : overlays_) overlay->finish();
        } else hide();
    }
    if (!actionable) return;
    Q_EMIT recognized(recognition.key, match->encode());
    if (match->kind == GestureAction::Kind::Open ||
        (match->kind == GestureAction::Kind::Command && match->value == QStringLiteral("open_exe_dir"))) {
        QString target = match->value;
        if (match->kind == GestureAction::Kind::Command) {
            auto directory = gestureProgramDirectory(frame.target);
            if (!directory) { Q_EMIT failed(directory.error()); return; }
            target = *directory;
        }
        LaunchItem item;
        item.target = target;
        const auto generation = input_.generation();
        const auto cancellation = input_.cancellation();
        jobs_->launch(item, root_, [this, generation, cancellation](auto result) {
            if (!result && generation == input_.generation() && cancellation == input_.cancellation())
                Q_EMIT failed(result.error());
        }, [this, generation, cancellation, frame] {
            return settings_.gesturesEnabled && generation == input_.generation() &&
                   cancellation == input_.cancellation() && !blocked_() &&
                   GestureInput::isCurrent(frame);
        });
    } else if (useAdmin) {
        admin_->gesture(*match, frame.target, frame.foregroundAtStart, this, [this](auto result) {
            if (!result)
                Q_EMIT failed(result.error());
        });
    } else if (auto result = actions_.execute(*match, frame.target, frame.foregroundAtStart);
               !result)
        Q_EMIT failed(result.error());
}
}
