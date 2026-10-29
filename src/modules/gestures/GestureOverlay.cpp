#include "GestureOverlay.h"
#include "ui/components/Theme.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace mt {
namespace {
constexpr qreal trailWidth = 3;
constexpr qreal trailLength = 2500;
constexpr qreal arrowLength = 17;

// Keep a bounded visible tail without changing the recognizer's full stroke.
QPolygonF visibleTail(const QPolygonF& points, bool& includesStart) {
    QPolygonF tail;
    tail.reserve(points.size());
    tail.append(points.last());
    qreal remaining = trailLength;
    includesStart = true;
    for (qsizetype i = points.size() - 1; i > 0; --i) {
        const QLineF segment(points[i], points[i - 1]);
        if (segment.length() > remaining) {
            tail.append(segment.pointAt(remaining / segment.length()));
            includesStart = false;
            break;
        }
        tail.append(points[i - 1]);
        remaining -= segment.length();
    }
    std::reverse(tail.begin(), tail.end());
    return tail;
}

void drawTrajectory(QPainter& painter, const QPolygonF& points, const QColor& color) {
    bool includesStart = false;
    const auto tail = visibleTail(points, includesStart);
    QPainterPath line(tail.first());
    qreal length = 0;
    for (qsizetype i = 1; i < tail.size(); ++i)
        length += QLineF(tail[i - 1], tail[i]).length();
    if (length < 0.01) return;

    // Use a length of the trailing path to orient the head, so a tiny final
    // mouse sample cannot turn the arrow sideways. The shaft ends at its base.
    qreal shaftRemaining = length - qMin(arrowLength, length * 0.75);
    for (qsizetype i = 1; i < tail.size(); ++i) {
        const QLineF segment(tail[i - 1], tail[i]);
        if (segment.length() > shaftRemaining) {
            line.lineTo(segment.pointAt(shaftRemaining / segment.length()));
            break;
        }
        line.lineTo(tail[i]);
        shaftRemaining -= segment.length();
    }
    painter.setPen(QPen(color, trailWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    if (includesStart) painter.drawEllipse(tail.first(), 4, 4);
    const auto base = line.currentPosition();
    const auto direction = tail.last() - base;
    const QPointF side(-direction.y() * 0.5, direction.x() * 0.5);
    painter.drawPolygon(QPolygonF{tail.last(), base + side, base - side});
}

// Rasterize only the small visible patch, at up to twice the physical DPI.
// Transparent text cannot use RGB ClearType; supersampled grayscale coverage
// keeps both Chinese glyphs and diagonal strokes smooth on any desktop colour.
void smoothPatch(QPainter& target, QRectF area, const QRect& clip, qreal dpr,
                 const std::function<void(QPainter&)>& draw) {
    area = area.intersected(QRectF(clip));
    if (area.isEmpty()) return;
    const QRect pixelBounds(qFloor(area.left() * dpr), qFloor(area.top() * dpr),
                            qCeil(area.right() * dpr) - qFloor(area.left() * dpr),
                            qCeil(area.bottom() * dpr) - qFloor(area.top() * dpr));
    area = QRectF(pixelBounds.x() / dpr, pixelBounds.y() / dpr,
                  pixelBounds.width() / dpr, pixelBounds.height() / dpr);
    const qreal pixels = qreal(pixelBounds.width()) * pixelBounds.height();
    if (pixels <= 0) return;
    if (pixels > 2000000) { draw(target); return; } // bounded temporary surface
    const qreal oversample = qMin(2.0, std::sqrt(2000000.0 / pixels));
    const QSize large(qCeil(pixelBounds.width() * oversample), qCeil(pixelBounds.height() * oversample));
    QImage surface(large, QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    {
        QPainter painter(&surface);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        painter.scale(large.width() / area.width(), large.height() / area.height());
        painter.translate(-area.topLeft());
        draw(painter);
    }
    auto resolved = surface.scaled(pixelBounds.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    resolved.setDevicePixelRatio(dpr);
    target.drawImage(area.topLeft(), resolved);
}
} // namespace

GestureOverlay::GestureOverlay(Theme& theme)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus), theme_(theme) {
    setObjectName(QStringLiteral("gestureOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    // Keep the same OS message font as the rest of mt; do not change the CJK
    // fallback by forcing a Latin family on this surface.
    auto actionFont = QApplication::font();
    actionFont.setPixelSize(26);
    actionFont.setLetterSpacing(QFont::AbsoluteSpacing, 4);
    actionFont.setHintingPreference(QFont::PreferNoHinting);
    actionFont.setStyleStrategy(QFont::PreferAntialias);
    setFont(actionFont);
    fadeTimer_.setInterval(16);
    connect(&fadeTimer_, &QTimer::timeout, this, [this] {
        opacity_ = qMax(0.0, 1.0 - fadeClock_.elapsed() / 150.0);
        if (opacity_ == 0) dismiss();
        else update();
    });
    connect(&theme_, &Theme::changed, this, qOverload<>(&GestureOverlay::update));
}
void GestureOverlay::present(const QPolygonF& desktopPoints, const QString& label, bool trail, bool matched) {
    if (desktopPoints.size() < 2 || (!trail && label.isEmpty())) {
        dismiss();
        return;
    }
    points_ = desktopPoints.translated(-geometry().topLeft());
    label_ = label;
    trail_ = trail;
    matched_ = matched;
    fadeTimer_.stop();
    opacity_ = 1;
    if (isVisible()) {
        update();
    } else {
        show();
        // Paint immediately if Windows has already exposed the window. If
        // exposure is asynchronous, its backing store is still transparent.
        repaint();
    }
}
void GestureOverlay::finish() {
    if (!isVisible() || fadeTimer_.isActive()) return;
    fadeClock_.start();
    fadeTimer_.start();
}
void GestureOverlay::dismiss() {
    fadeTimer_.stop();
    opacity_ = 1;
    points_.clear();
    label_.clear();
    if (isVisible()) repaint();
    hide();
}
void GestureOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (points_.isEmpty()) return;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity_);
    const auto color = theme_.color(QStringLiteral("gesture-trail"));
    if (trail_) {
        bool includesStart = false;
        const auto tail = visibleTail(points_, includesStart);
        smoothPatch(painter, tail.boundingRect().adjusted(-12, -12, 12, 12), rect(), devicePixelRatioF(),
                    [&](QPainter& patch) { drawTrajectory(patch, points_, color); });
    }
    if (label_.isEmpty() || !QRectF(rect()).contains(points_.last())) return;
    const auto textFont = font();
    const auto text = QFontMetricsF(textFont).elidedText(label_, Qt::ElideMiddle, qMax(1, width() - 40));
    QPainterPath glyphs;
    glyphs.addText({}, textFont, text);
    const auto bounds = glyphs.boundingRect();
    glyphs.translate((width() - bounds.width()) / 2 - bounds.left(),
                     qMax(8, height() - 150) - bounds.top());
    // Outline first, then fill: drawPath's fill-before-stroke order would cover
    // the narrow white strokes of Chinese glyphs at the original 26px size.
    smoothPatch(painter, glyphs.boundingRect().adjusted(-3, -3, 3, 3), rect(), devicePixelRatioF(),
                [&](QPainter& patch) {
        patch.strokePath(glyphs, QPen(matched_ ? color : theme_.color(QStringLiteral("gesture-unmatched")),
                                     3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        patch.fillPath(glyphs, theme_.color(QStringLiteral("gesture-text")));
    });
}
}
