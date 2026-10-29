#include "LockPassword.h"
#include "infrastructure/i18n/I18n.h"
#include <QByteArray>
#include <QScopeGuard>
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on

namespace mt {
std::expected<QString, QString> protectLockPassword(const QString& plain) {
    auto bytes = plain.toUtf8();
    DATA_BLOB in{DWORD(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, out{};
    const bool ok = CryptProtectData(&in, L"mt blackout", nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out);
    SecureZeroMemory(bytes.data(), size_t(bytes.size()));
    if (!ok)
        return std::unexpected(tx(u"无法保存解锁密码"));
    const auto cleanup = qScopeGuard([&] { LocalFree(out.pbData); });
    const auto result =
        QStringLiteral("dpapi:") +
        QString::fromLatin1(QByteArray(reinterpret_cast<char*>(out.pbData), out.cbData).toBase64());
    return result;
}
std::expected<QString, QString> revealLockPassword(const QString& value) {
    if (!value.startsWith(QStringLiteral("dpapi:")) || value.size() > 16384)
        return std::unexpected(tx(u"解锁密码无法读取，请在设置中重新设置密码"));
    auto bytes =
        QByteArray::fromBase64(value.mid(6).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    DATA_BLOB in{DWORD(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, out{};
    if (bytes.isEmpty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::unexpected(tx(u"解锁密码无法读取，请在设置中重新设置密码"));
    const auto cleanup = qScopeGuard([&] {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    });
    auto result = QString::fromUtf8(reinterpret_cast<char*>(out.pbData), out.cbData);
    return result;
}
} // namespace mt
