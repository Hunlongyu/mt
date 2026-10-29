#pragma once
#include <QPoint>
#include <array>
#include <span>

namespace mt {
// Allocation-free state used at the native callback boundary. A swallowed press
// owns its release even after cancellation, timeout or module disable.
class GestureCapture {
  public:
    enum class Result { Pending, Click, Stroke, Cancelled };
    bool begin(QPoint point, quint64 now, int triggerDistance = 5) noexcept;
    void move(QPoint point, quint64 now) noexcept;
    void cancel(bool preserveClick = false) noexcept;
    void expire(quint64 now, int timeout) noexcept;
    Result release(QPoint point, quint64 now) noexcept;
    bool held() const noexcept { return held_; }
    bool drawing() const noexcept { return drawing_; }
    bool cancelled() const noexcept { return cancelled_; }
    quint64 revision() const noexcept { return revision_; }
    std::span<const QPoint> points() const noexcept { return {points_.data(), size_}; }
    Result result() const noexcept { return result_; }
    void reset() noexcept;

  private:
    std::array<QPoint, 4096> points_{};
    size_t size_ = 0;
    quint64 lastMove_ = 0;
    quint64 revision_ = 0;
    int trigger_ = 5;
    bool held_ = false;
    bool drawing_ = false;
    bool cancelled_ = false;
    bool restoreClick_ = false;
    Result result_ = Result::Pending;
};
} // namespace mt
