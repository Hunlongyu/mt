#pragma once
#include <QPoint>
#include <QRect>
#include <QSize>

namespace mt {
// Top-left for a popup of `size` next to a text caret, kept entirely inside
// `area` (the caret's own screen). Below the caret line when it fits, above it
// when that fits, otherwise on the roomier side clamped to the screen; never
// onto a neighbouring screen, whose scale factor would resize the popup.
QPoint placeBesideCaret(const QRect& caret, const QSize& size, const QRect& area, int gap = 4);
} // namespace mt
