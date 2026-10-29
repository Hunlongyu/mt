#include "HLaunchImport.h"
#include "infrastructure/i18n/I18n.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <optional>

namespace mt {
namespace {
constexpr qint64 maxFileSize = 8 * 1024 * 1024;
std::expected<QJsonObject, QString> readObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(file.errorString());
    if (file.size() > maxFileSize)
        return std::unexpected(tx(u"HLaunch 数据文件异常大，未导入"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(tx(u"HLaunch 数据文件解析失败"));
    return document.object();
}
// HLaunch resolves ordinary relative paths against its EXE folder. Drive-relative
// ("C:tool.exe") and root-relative ("\tool.exe") paths depend on the current
// directory, which HLaunch itself rejects; they cannot be imported faithfully.
std::optional<QString> absolutePath(const QString& path, const QDir& exeDir) {
    if (path.isEmpty() || path.contains(u'%'))
        return path; // environment variables are expanded at launch time
    const bool driveRelative = path.size() >= 2 && path[1] == u':' &&
                               (path.size() == 2 || (path[2] != u'\\' && path[2] != u'/'));
    const bool rootRelative =
        (path.startsWith(u'\\') || path.startsWith(u'/')) && !path.startsWith(QStringLiteral("\\\\")) &&
        !path.startsWith(QStringLiteral("//"));
    if (driveRelative || rootRelative)
        return std::nullopt;
    if (QDir::isAbsolutePath(path))
        return QDir::toNativeSeparators(path);
    return QDir::toNativeSeparators(QDir::cleanPath(exeDir.absoluteFilePath(path)));
}
QString nullableText(const QJsonValue& value) {
    return value.isString() ? value.toString() : QString{};
}
} // namespace
QString defaultHLaunchFolder() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("HLaunch"));
}
std::expected<HLaunchData, QString> readHLaunchData(const QString& folder,
                                                    const QString& dataRoot, int targetColumns) {
    QDir dataDir(folder);
    if (!dataDir.exists(QStringLiteral("items.json")) &&
        QDir(dataDir.filePath(QStringLiteral("data"))).exists(QStringLiteral("items.json")))
        dataDir.setPath(dataDir.filePath(QStringLiteral("data")));
    if (!dataDir.exists(QStringLiteral("items.json")))
        return std::unexpected(tx(u"所选文件夹中没有 HLaunch 的 items.json"));
    // Portable HLaunch keeps data/ next to the EXE; otherwise the folder is the base.
    QDir exeDir = dataDir;
    if (dataDir.dirName().compare(QStringLiteral("data"), Qt::CaseInsensitive) == 0)
        exeDir.cdUp();
    const auto root = readObject(dataDir.filePath(QStringLiteral("items.json")));
    if (!root)
        return std::unexpected(root.error());
    if (!root->value(QStringLiteral("schemaVersion")).isDouble() ||
        root->value(QStringLiteral("schemaVersion")).toInt() != 1)
        return std::unexpected(tx(u"HLaunch 数据版本不受支持，未导入"));
    int columns = 5; // HLaunch's default grid
    if (dataDir.exists(QStringLiteral("config.json")))
        if (const auto config = readObject(dataDir.filePath(QStringLiteral("config.json"))))
            columns = std::clamp(config->value(QStringLiteral("appearance"))
                                     .toObject()
                                     .value(QStringLiteral("gridColumns"))
                                     .toInt(columns),
                                 3, 20);
    const auto shortcutsDir =
        QDir::cleanPath(dataDir.filePath(QStringLiteral("shortcuts"))).toCaseFolded();
    HLaunchData out;
    const auto tabs = root->value(QStringLiteral("tabs")).toArray();
    for (const auto& tabValue : tabs) {
        const auto tab = tabValue.toObject();
        ImportedCategory category;
        category.name = tab.value(QStringLiteral("name")).toString().trimmed();
        if (category.name.isEmpty())
            category.name = tx(u"导入");
        QList<QPair<int, LaunchItem>> inGrid, overflow;
        QSet<int> usedSlots;
        int nextFree = 0;
        const auto entries = tab.value(QStringLiteral("items")).toArray();
        for (const auto& entryValue : entries) {
            const auto o = entryValue.toObject();
            const auto type = o.value(QStringLiteral("type")).toString();
            LaunchItem item;
            item.id = o.value(QStringLiteral("id")).toString();
            item.name = o.value(QStringLiteral("name")).toString().trimmed();
            item.target = o.value(QStringLiteral("target")).toString().trimmed();
            item.elevated = o.value(QStringLiteral("runAsAdministrator")).toBool();
            item.launchCount =
                std::max<qint64>(0, static_cast<qint64>(o.value(QStringLiteral("launchCount")).toDouble()));
            item.lastLaunched = QDateTime::fromString(
                nullableText(o.value(QStringLiteral("lastLaunchedAt"))), Qt::ISODate);
            static const QStringList types{QStringLiteral("application"), QStringLiteral("file"),
                                           QStringLiteral("folder"), QStringLiteral("url"),
                                           QStringLiteral("shortcut")};
            if (item.name.isEmpty() || item.target.isEmpty() || !types.contains(type)) {
                ++out.skipped;
                continue;
            }
            // HLaunch stores one logical argument per array element.
            QStringList arguments;
            for (const auto& argument : o.value(QStringLiteral("arguments")).toArray())
                arguments << LauncherCatalog::quoteArgument(argument.toString());
            item.arguments = arguments.join(u' ');
            const auto cwd =
                absolutePath(nullableText(o.value(QStringLiteral("workingDirectory"))), exeDir);
            const auto icon = absolutePath(nullableText(o.value(QStringLiteral("icon"))), exeDir);
            const auto target = type == QStringLiteral("url")
                                    ? std::optional<QString>(item.target)
                                    : absolutePath(item.target, exeDir);
            if (!cwd || !icon || !target) {
                ++out.skipped;
                continue;
            }
            item.workingDirectory = *cwd;
            item.iconPath = *icon;
            item.target = *target;
            // Shortcut copies HLaunch kept as user data move into this data root,
            // so the import never depends on the old installation.
            if (type == QStringLiteral("shortcut") &&
                QDir::cleanPath(QFileInfo(item.target).absolutePath()).toCaseFolded() ==
                    shortcutsDir) {
                const auto destination = QDir(dataRoot).filePath(QStringLiteral("shortcuts"));
                QFile source(item.target);
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!source.open(QIODevice::ReadOnly) || source.size() > maxFileSize ||
                    !hash.addData(&source)) {
                    ++out.skipped;
                    continue;
                }
                const auto copy = QDir(destination).filePath(
                    QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".lnk"));
                source.close();
                // Content identity survives a second complete read/merge import.
                bool present = false;
                if (QFile existing(copy); existing.open(QIODevice::ReadOnly)) {
                    QCryptographicHash existingHash(QCryptographicHash::Sha256);
                    present = existing.size() <= maxFileSize && existingHash.addData(&existing) &&
                              existingHash.result() == hash.result();
                }
                if (!present && (!QDir().mkpath(destination) || !QFile::copy(item.target, copy))) {
                    ++out.skipped;
                    continue;
                }
                item.target = QDir::toNativeSeparators(copy);
            }
            int slot = -1;
            if (const auto value = o.value(QStringLiteral("gridSlot")); value.isDouble())
                slot = value.toInt(-1);
            if (slot < 0 || slot > 9999 || usedSlots.contains(slot)) {
                // Missing legacy slots follow array order, as in HLaunch.
                while (usedSlots.contains(nextFree))
                    ++nextFree;
                slot = nextFree;
            }
            usedSlots.insert(slot);
            // Keep each item's row and column; items in columns beyond the last
            // continue from the start of the next row.
            const int row = slot / columns;
            const int column = slot % columns;
            if (column < targetColumns)
                inGrid.append({row * targetColumns + column, item});
            else
                overflow.append({(row + 1) * targetColumns, item});
        }
        category.items = inGrid + overflow;
        out.categories.append(category);
    }
    return out;
}
} // namespace mt
