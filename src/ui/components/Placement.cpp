#include "Placement.h"
#include <algorithm>

namespace mt {
QPoint placeBesideCaret(const QRect& caret, const QSize& size, const QRect& area, int gap) {
    const int below = caret.bottom() + 1 + gap;
    const int above = caret.top() - gap - size.height();
    const int roomBelow = area.bottom() + 1 - below;
    const int roomAbove = caret.top() - gap - area.top();
    int y = below;
    int x = caret.left();
    if (roomBelow >= size.height()) {
        y = below;
    } else if (roomAbove >= size.height()) {
        y = above;
    } else {
        // A short screen: neither side fits. Sit beside the caret instead of
        // covering the line being typed, vertically centred on it.
        y = caret.center().y() - size.height() / 2;
        const int right = caret.right() + 1 + gap;
        const int left = caret.left() - gap - size.width();
        if (right + size.width() <= area.right() + 1)
            return {right, std::clamp(y, area.top(), std::max(area.top(), area.bottom() + 1 - size.height()))};
        if (left >= area.left())
            return {left, std::clamp(y, area.top(), std::max(area.top(), area.bottom() + 1 - size.height()))};
        y = roomAbove > roomBelow ? above : below;
    }
    if (x + size.width() > area.right() + 1)
        x = caret.right() + 1 - size.width(); // right-align with the caret instead
    // Whatever side was chosen, stay on this screen.
    x = std::clamp(x, area.left(), std::max(area.left(), area.right() + 1 - size.width()));
    y = std::clamp(y, area.top(), std::max(area.top(), area.bottom() + 1 - size.height()));
    return {x, y};
}
} // namespace mt
