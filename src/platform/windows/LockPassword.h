#pragma once
#include <QString>
#include <expected>

namespace mt {
// Current Windows user's DPAPI, not a portable credential or an account lock.
std::expected<QString, QString> protectLockPassword(const QString& plain);
std::expected<QString, QString> revealLockPassword(const QString& protectedValue);
} // namespace mt
