#include "GestureCapture.h"
#include <algorithm>
#include <cmath>

namespace mt {
void GestureCapture::reset() noexcept {
    size_ = 0;
    held_ = drawing_ = cancelled_ = restoreClick_ = false;
    result_ = Result::Pending;
    ++revision_;
}
bool GestureCapture::begin(QPoint point, quint64 now, int distance) noexcept {
    if (held_)
        return false;
    reset();
    held_ = true;
    trigger_ = std::clamp(distance, 2, 100);
    points_[size_++] = point;
    lastMove_ = now;
    return true;
}
void GestureCapture::move(QPoint point, quint64 now) noexcept {
    if (!held_ || cancelled_)
        return;
    const auto delta = point - points_[size_ - 1];
    if (std::hypot(double(delta.x()), double(delta.y())) < 2.0)
        return;
    lastMove_ = now;
    if (size_ == points_.size()) {
        cancel(); // never classify a truncated stroke as a different gesture
        return;
    }
    points_[size_++] = point;
    const auto fromStart = point - points_[0];
    drawing_ |= std::hypot(double(fromStart.x()), double(fromStart.y())) >= trigger_;
    ++revision_;
}
void GestureCapture::cancel(bool preserveClick) noexcept {
    if (!held_)
        return;
    if (cancelled_) {
        if (!preserveClick) restoreClick_ = false;
        return;
    }
    cancelled_ = true;
    restoreClick_ = preserveClick && !drawing_;
    ++revision_;
}
void GestureCapture::expire(quint64 now, int timeout) noexcept {
    if (held_ && timeout > 0 && now >= lastMove_ && now - lastMove_ >= quint64(timeout))
        cancel(true);
}
GestureCapture::Result GestureCapture::release(QPoint point, quint64 now) noexcept {
    if (!held_)
        return Result::Pending;
    move(point, now);
    held_ = false;
    result_ = cancelled_ ? (restoreClick_ ? Result::Click : Result::Cancelled)
                         : (drawing_ ? Result::Stroke : Result::Click);
    ++revision_;
    return result_;
}
} // namespace mt
