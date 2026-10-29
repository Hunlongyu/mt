#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVersionNumber>
#include <expected>
#include <optional>

namespace mt {
struct UpdateRelease {
    QString version;
    QUrl url;
    QByteArray sha256;
    qint64 bytes = 0;
};
inline constexpr qint64 maxUpdateBytes = 200 * 1024 * 1024;
std::optional<QVersionNumber> stableVersion(const QString& text);
std::expected<std::optional<UpdateRelease>, QString> parseUpdateRelease(
    const QByteArray& json, const QString& currentVersion);
bool isUpdateDownloadUrl(const QUrl& url, bool redirected = false);
} // namespace mt
