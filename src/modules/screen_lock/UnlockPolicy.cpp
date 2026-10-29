#include "UnlockPolicy.h"
#include "infrastructure/i18n/I18n.h"
#include <QCryptographicHash>

namespace mt {
bool UnlockPolicy::validPassword(const QString& p) {
    if (p.isEmpty() || p.size() > 128)
        return false;
    for (const auto ch : p)
        if (!((ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
              (ch >= u'0' && ch <= u'9')))
            return false;
    return true;
}
std::expected<void, QString> UnlockPolicy::configure(bool required, const QString& password) {
    if (required && !validPassword(password))
        return std::unexpected(tx(u"密码须为 1–128 位英文字母或数字，区分大小写"));
    required_ = required;
    digest_ = required ? QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256)
                       : QByteArray{};
    clear();
    return {};
}
bool UnlockPolicy::append(QChar ch) {
    if (!required_)
        return false;
    if (input_.size() >= 128) {
        overflow_ = true;
        return false;
    }
    if (!validPassword(QString(ch)))
        return false;
    input_.append(ch);
    return true;
}
void UnlockPolicy::backspace() {
    if (!input_.isEmpty())
        input_.chop(1);
    overflow_ = false;
}
void UnlockPolicy::clear() {
    input_.fill(QChar{});
    input_.clear();
    overflow_ = false;
}
bool UnlockPolicy::submit() {
    if (!required_)
        return true;
    const auto candidate = QCryptographicHash::hash(input_.toUtf8(), QCryptographicHash::Sha256);
    unsigned diff = overflow_ ? 1u : 0u;
    for (int i = 0; i < digest_.size(); ++i)
        diff |= static_cast<unsigned char>(digest_[i]) ^ static_cast<unsigned char>(candidate[i]);
    clear();
    return diff == 0;
}
} // namespace mt
