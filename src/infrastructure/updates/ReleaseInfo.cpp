#include "ReleaseInfo.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>

namespace mt {
std::optional<QVersionNumber> stableVersion(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^v?(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return {};
    return QVersionNumber(match.captured(1).toInt(), match.captured(2).toInt(),
                          match.captured(3).toInt());
}
bool isUpdateDownloadUrl(const QUrl& url, bool redirected) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        !url.userInfo().isEmpty() || url.port(443) != 443 || !url.fragment().isEmpty())
        return false;
    if (url.host() == QStringLiteral("github.com"))
        return url.path().startsWith(QStringLiteral("/Hunlongyu/mt/releases/download/"),
                                     Qt::CaseInsensitive);
    return redirected && (url.host() == QStringLiteral("release-assets.githubusercontent.com") ||
                           url.host() == QStringLiteral("objects.githubusercontent.com"));
}
std::expected<std::optional<UpdateRelease>, QString> parseUpdateRelease(
    const QByteArray& json, const QString& currentVersion) {
    if (json.size() > 1024 * 1024)
        return std::unexpected(QStringLiteral("版本信息超过大小限制"));
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(QStringLiteral("GitHub 返回的版本信息无效"));
    const auto object = doc.object();
    if (!object.value(QStringLiteral("draft")).isBool() ||
        !object.value(QStringLiteral("prerelease")).isBool())
        return std::unexpected(QStringLiteral("版本发布状态无效"));
    if (object.value(QStringLiteral("draft")).toBool() ||
        object.value(QStringLiteral("prerelease")).toBool())
        return std::optional<UpdateRelease>{};
    const auto tag = object.value(QStringLiteral("tag_name")).toString();
    const auto version = stableVersion(tag);
    const auto current = stableVersion(currentVersion);
    if (!version || !current)
        return std::unexpected(QStringLiteral("只支持三段式正式版本号"));
    if (QVersionNumber::compare(*version, *current) <= 0)
        return std::optional<UpdateRelease>{};
    std::optional<UpdateRelease> found;
    for (const auto& value : object.value(QStringLiteral("assets")).toArray()) {
        const auto asset = value.toObject();
        if (asset.value(QStringLiteral("name")).toString() != QStringLiteral("mt-windows-x64.exe"))
            continue;
        if (found)
            return std::unexpected(QStringLiteral("发布中存在重复的更新文件"));
        const auto digest = asset.value(QStringLiteral("digest")).toString();
        static const QRegularExpression hash(QStringLiteral("^sha256:([a-fA-F0-9]{64})$"));
        const auto matched = hash.match(digest);
        const auto bytes = asset.value(QStringLiteral("size")).toDouble(-1);
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
        const QString expectedPath = QStringLiteral("/Hunlongyu/mt/releases/download/%1/mt-windows-x64.exe").arg(tag);
        if (!matched.hasMatch() || !std::isfinite(bytes) || bytes < 1 ||
            bytes > maxUpdateBytes || bytes != std::floor(bytes) ||
            asset.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded") ||
            !isUpdateDownloadUrl(url) || url.path() != expectedPath || url.hasQuery())
            return std::unexpected(QStringLiteral("更新文件的地址、大小或 SHA-256 校验信息无效"));
        found = UpdateRelease{version->toString(), url,
                              QByteArray::fromHex(matched.captured(1).toLatin1()),
                              static_cast<qint64>(bytes)};
    }
    if (!found)
        return std::unexpected(QStringLiteral("此版本没有 Windows x64 更新文件，请查看 GitHub 发布页"));
    return found;
}
} // namespace mt
