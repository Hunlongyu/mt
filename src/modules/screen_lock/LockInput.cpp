#include "LockInput.h"

namespace mt {
std::expected<void, QString> LockInput::begin(bool required, const QString& password,
                                              const QKeySequence& shortcut) {
    if (auto result = policy_.configure(required, password); !result)
        return result;
    shortcut_ = shortcut.isEmpty() ? QKeyCombination{} : shortcut[0];
    return {};
}
LockInput::Result LockInput::press(int key, Qt::KeyboardModifiers mods, QChar ch, bool repeat) {
    if (repeat)
        return Result::Ignored;
    mods &= Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (QKeyCombination(mods, Qt::Key(key)) == shortcut_)
        return policy_.passwordRequired() ? Result::Ignored : Result::Unlock;
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return Result::Ignored;
    if (key == Qt::Key_Escape) {
        policy_.clear();
    } else if (key == Qt::Key_Backspace) {
        policy_.backspace();
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        return policy_.submit() ? Result::Unlock : Result::Rejected;
    } else if (!ch.isNull()) {
        policy_.append(ch);
    }
    return Result::Ignored;
}
} // namespace mt
