#include "Panel.h"
#include "Theme.h"
#include "platform/windows/MenuFrame.h"
#include <QApplication>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

namespace mt {
Panel::Panel(Theme& theme, const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      theme_(theme), frame_(std::make_unique<MenuFrame>()) {
    setObjectName(QStringLiteral("PanelSurface"));
    // Opaque on purpose: DWM gives per-pixel-alpha windows neither the system
    // shadow nor rounded corners. Uniform window opacity remains allowed.
    setAttribute(Qt::WA_StyledBackground);
    setWindowTitle(title);
    setAccessibleName(title);
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 0, 0);
    column_->setSpacing(0);
    connect(&theme, &Theme::changed, this, [this] {
        if (isVisible())
            refreshFrame();
    });
    fade_ = new QVariantAnimation(this);
    fade_->setDuration(Theme::motionMs);
    fade_->setEasingCurve(QEasingCurve::OutCubic);
    connect(fade_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { applyOpacity(opacity_ * v.toReal()); });
}
Panel::~Panel() = default;
void Panel::refreshFrame() {
    if (auto* window = windowHandle(); window && window->handle())
        frame_->apply(*window, theme_.name() == QStringLiteral("dark"), false);
}
void Panel::applyOpacity(qreal value) {
    setWindowOpacity(value);
}
void Panel::paintEvent(QPaintEvent*) {
    // The shared stylesheet paints the surface; DWM clips and frames the corners.
    QStyleOption option;
    option.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
}
void Panel::setPanelOpacity(int percent) {
    const qreal value = qBound(30, percent, 100) / 100.0;
    opacity_ = value;
    fade_->stop();
    applyOpacity(value);
    Q_EMIT opacityChanged(value);
}
void Panel::showEvent(QShowEvent* event) {
    // Frame the native window before it is mapped so the first frame already has
    // the system corners and shadow.
    (void)winId();
    refreshFrame();
    // QShowEvent arrives before the native window is mapped, so starting at zero
    // avoids a full-opacity first frame.
    if (animated_ && !property("reconfiguring").toBool()) {
        fade_->stop();
        applyOpacity(0.0);
        fade_->setStartValue(0.0);
        fade_->setEndValue(1.0);
        fade_->start();
    }
    QWidget::showEvent(event);
}
void Panel::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
}
void Panel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}
void Panel::showNear(QPoint anchor, bool activate) {
    auto* screen = QGuiApplication::screenAt(anchor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const auto available = screen->availableGeometry();
        anchor.setX(qBound(available.left(), anchor.x(),
                           qMax(available.left(), available.right() - width() + 1)));
        anchor.setY(qBound(available.top(), anchor.y(),
                           qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(anchor);
    setAttribute(Qt::WA_ShowWithoutActivating, !activate);
    show();
    raise();
    if (activate)
        activateWindow();
}
void Panel::enableTitleDrag(QWidget* widget) {
    if (dragWidgets_.contains(widget))
        return;
    dragWidgets_.insert(widget);
    widget->installEventFilter(this);
}
bool Panel::eventFilter(QObject* watched, QEvent* event) {
    if (dragWidgets_.contains(watched) && handleSurfaceDrag(static_cast<QWidget*>(watched), event))
        return true;
    return QWidget::eventFilter(watched, event);
}
bool Panel::handleSurfaceDrag(QWidget* surface, QEvent* event, bool allowStart) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (allowStart && e->button() == Qt::LeftButton) {
            dragSource_ = surface;
            dragOffset_ = e->globalPosition().toPoint() - pos();
            return true;
        }
    }
    if (!dragSource_ || dragSource_ != surface)
        return false;
    if (event->type() == QEvent::MouseMove) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (!(e->buttons() & Qt::LeftButton)) {
            dragSource_.clear();
            return false;
        }
        move(e->globalPosition().toPoint() - dragOffset_);
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        dragSource_.clear();
        return true;
    }
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
        dragSource_.clear();
    return false;
}
bool Panel::event(QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate)
        dragSource_.clear();
    return QWidget::event(event);
}
void Panel::closeEvent(QCloseEvent* event) {
    // quitOnLastWindowClosed is disabled by the host. Accepting close hides this reusable
    // panel without terminating the app, and allows an explicit application quit to finish.
    event->accept();
    hide();
}
void Panel::hideEvent(QHideEvent* event) {
    dragSource_.clear();
    if (fade_->state() == QAbstractAnimation::Running) {
        fade_->stop();
        applyOpacity(opacity_);
    }
    QWidget::hideEvent(event);
    if (!property("reconfiguring").toBool())
        Q_EMIT dismissed();
}
} // namespace mt
