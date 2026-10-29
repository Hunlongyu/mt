#pragma once
#include "UnlockPolicy.h"
#include <QKeySequence>

namespace mt {
// One lock session takes a snapshot of its mode, password and shortcut.
class LockInput {
  public:
    enum class Result { Ignored, Rejected, Unlock };
    std::expected<void, QString> begin(bool required, const QString& password,
                                      const QKeySequence& shortcut);
    Result press(int key, Qt::KeyboardModifiers modifiers, QChar character, bool repeat);
    void clear() { policy_.clear(); }
  private:
    UnlockPolicy policy_;
    QKeyCombination shortcut_;
};
} // namespace mt
