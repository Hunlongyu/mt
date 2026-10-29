#include "ClipboardText.h"
#include "infrastructure/i18n/I18n.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace mt {
namespace {
QColor hexColor(QStringView digits) {
    auto expand = [](QStringView s) {
        QString out;
        for (const auto c : s)
            out += QString(2, c);
        return out;
    };
    const QString full = digits.size() <= 4 ? expand(digits) : digits.toString();
    bool ok = false;
    const auto value = full.toUInt(&ok, 16);
    if (!ok)
        return {};
    if (full.size() == 6)
        return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    if (full.size() == 8) // #RRGGBBAA, the CSS order
        return QColor((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF,
                      value & 0xFF);
    return {};
}
} // namespace
QColor clipColor(const QString& text) {
    // A colour is something copied as a short token, perhaps with a little CSS
    // around it; long text that merely mentions a colour never gets a swatch.
    if (text.size() > 32)
        return {};
    static const QRegularExpression functional(
        QStringLiteral(R"(rgba?\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*(?:,\s*(\d*\.?\d+)\s*)?\))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hashed(
        QStringLiteral(R"(#([0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3,4})(?![0-9a-fA-F]))"));
    static const QRegularExpression bare(
        QStringLiteral(R"((?<![0-9a-zA-Z#])([0-9a-fA-F]{8}|[0-9a-fA-F]{6})(?![0-9a-zA-Z]))"));
    QColor found;
    qsizetype at = text.size();
    if (const auto m = functional.match(text); m.hasMatch()) {
        const int r = m.captured(1).toInt(), g = m.captured(2).toInt(), b = m.captured(3).toInt();
        if (r <= 255 && g <= 255 && b <= 255) {
            found = QColor(r, g, b);
            if (m.hasCaptured(4))
                found.setAlphaF(std::clamp(m.captured(4).toDouble(), 0.0, 1.0));
            at = m.capturedStart();
        }
    }
    for (const auto* pattern : {&hashed, &bare})
        if (const auto m = pattern->match(text); m.hasMatch() && m.capturedStart() < at) {
            found = hexColor(m.capturedView(1));
            at = m.capturedStart();
        }
    return found;
}
OpenTarget openTarget(const QString& text) {
    const auto s = text.trimmed();
    if (s.isEmpty() || s.contains(u'\n'))
        return {};
    const auto lower = s.toLower();
    for (const auto* scheme : {"http://", "https://", "ftp://"})
        if (lower.startsWith(QLatin1StringView(scheme)))
            return {OpenKind::Url, s};
    if (lower.startsWith(QStringLiteral("www.")) && !s.contains(u' '))
        return {OpenKind::Url, QStringLiteral("https://") + s};
    static const QRegularExpression email(QStringLiteral(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)"));
    if (email.match(s).hasMatch())
        return {OpenKind::Email, s};
    // Classified by shape only: probing a disconnected network path here would
    // stall the hover preview. Existence is checked when the user opens it.
    if (s.contains(u'/') || s.contains(u'\\') || QDir::isAbsolutePath(s))
        return {OpenKind::Path, QDir::cleanPath(s)};
    return {};
}
int clipCharacters(const QString& text) {
    qsizetype count = 0;
    for (qsizetype i = 0; i < text.size(); ++i, ++count)
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate())
            ++i;
    return static_cast<int>(std::min<qsizetype>(count, std::numeric_limits<int>::max()));
}
int clipLines(const QString& text) {
    return text.isEmpty() ? 0 : static_cast<int>(text.count(u'\n')) + 1;
}
QString clipRelativeTime(const QDateTime& captured, const QDateTime& now) {
    const auto seconds = captured.secsTo(now);
    if (seconds < 60)
        return tx(u"刚刚");
    if (seconds < 3600)
        return tx(u"%1 分钟前").arg(seconds / 60);
    const auto local = captured.toLocalTime();
    const auto today = now.toLocalTime().date();
    if (local.date() == today)
        return tx(u"%1 小时前").arg(seconds / 3600);
    if (local.date() == today.addDays(-1))
        return tx(u"昨天 %1").arg(local.toString(QStringLiteral("HH:mm")));
    return local.toString(local.date().year() == today.year() ? QStringLiteral("MM-dd")
                                                              : QStringLiteral("yyyy-MM-dd"));
}
} // namespace mt
