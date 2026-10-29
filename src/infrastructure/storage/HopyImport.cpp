#include "HopyImport.h"
#include "infrastructure/i18n/I18n.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>

namespace mt {
namespace {
// Hopy stores absolute sidecar paths; when the folder was moved or copied, the
// same file name under the given folder is the copy to use.
QString sidecar(const QString& stored, const QString& folder, const QString& subfolder) {
    if (stored.isEmpty())
        return {};
    if (QFileInfo::exists(stored))
        return stored;
    const auto moved = QDir(folder).filePath(subfolder + u'/' + QFileInfo(stored).fileName());
    return QFileInfo::exists(moved) ? moved : QString{};
}
class ReadOnlyConnection {
  public:
    explicit ReadOnlyConnection(const QString& path)
        : name_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db_.setDatabaseName(path);
        db_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    }
    ~ReadOnlyConnection() {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    QSqlDatabase& db() {
        return db_;
    }

  private:
    QString name_;
    QSqlDatabase db_;
};
} // namespace
QString defaultHopyFolder() {
    // Hopy uses QStandardPaths::AppDataLocation (the roaming profile on Windows)
    // with organization and application both named "hopy".
    return QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("hopy/hopy"));
}
std::expected<HopyHistory, QString> readHopyHistory(const QString& folder,
                                                   const ClipConsumer& consume) {
    const auto source = QDir(folder).filePath(QStringLiteral("clipboard.sqlite"));
    if (!QFileInfo(source).isFile())
        return std::unexpected(tx(u"所选文件夹中没有 Hopy 的 clipboard.sqlite"));
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return std::unexpected(tx(u"无法创建导入临时目录"));
    // Copy the main file and any write-ahead log so committed data is complete.
    for (const auto* suffix : {"", "-wal"}) {
        const auto from = source + QLatin1StringView(suffix);
        if (QFileInfo::exists(from) &&
            !QFile::copy(from, scratch.filePath(QStringLiteral("clipboard.sqlite")) +
                                   QLatin1StringView(suffix)))
            return std::unexpected(tx(u"无法复制 Hopy 历史数据库"));
    }
    ReadOnlyConnection connection(scratch.filePath(QStringLiteral("clipboard.sqlite")));
    auto& db = connection.db();
    if (!db.open())
        return std::unexpected(db.lastError().text());
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT value FROM meta WHERE key='schema_version'")) && q.next() &&
        q.value(0).toInt() > 1)
        return std::unexpected(tx(u"Hopy 历史数据库版本较新，暂不支持导入"));
    QSet<QString> columns;
    if (!q.exec(QStringLiteral("PRAGMA table_info(records)")))
        return std::unexpected(q.lastError().text());
    while (q.next())
        columns.insert(q.value(1).toString());
    for (const auto* required : {"content", "type", "created_at", "pinned", "favorite",
                                 "html_path", "image_path"})
        if (!columns.contains(QLatin1StringView(required)))
            return std::unexpected(tx(u"不是可识别的 Hopy 历史数据库"));
    const auto sensitive = columns.contains(QStringLiteral("sensitive"))
                               ? QStringLiteral("sensitive")
                               : QStringLiteral("0");
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT content,type,created_at,pinned,favorite,html_path,"
                               "image_path,%1 FROM records ORDER BY created_at")
                    .arg(sensitive)))
        return std::unexpected(q.lastError().text());
    HopyHistory history;
    while (q.next()) {
        ClipboardRecord r;
        const auto content = q.value(0).toString();
        r.captured = QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong(), QTimeZone::UTC);
        r.pinned = q.value(3).toInt() != 0;
        r.favorite = q.value(4).toInt() != 0;
        r.sensitive = q.value(7).toInt() != 0;
        switch (q.value(1).toInt()) {
        case 0:
            r.kind = ClipKind::Text;
            r.text = content;
            break;
        case 1: {
            r.kind = ClipKind::Html;
            r.text = content;
            QFile html(sidecar(q.value(5).toString(), folder, QStringLiteral("rich_text")));
            if (html.fileName().isEmpty() || !html.open(QIODevice::ReadOnly) ||
                html.size() > ClipboardRecord::maxHtml * 4)
                r.kind = ClipKind::Text; // keep the text even when formatting is gone
            else
                r.html = QString::fromUtf8(html.readAll());
            break;
        }
        case 2: {
            r.kind = ClipKind::Image;
            const auto path = sidecar(q.value(6).toString(), folder, QStringLiteral("images"));
            if (path.isEmpty()) {
                ++history.skipped;
                continue;
            }
            QImageReader reader(path);
            const auto size = reader.size();
            if (!size.isValid() ||
                size.width() > ClipboardRecord::maxImageSide ||
                size.height() > ClipboardRecord::maxImageSide ||
                qint64(size.width()) * size.height() * 4 > ClipboardRecord::maxImageBytes) {
                ++history.skipped;
                continue;
            }
            r.pendingImage = reader.read();
            if (r.pendingImage.isNull()) {
                ++history.skipped;
                continue;
            }
            break;
        }
        case 3:
            r.kind = ClipKind::Files;
            r.files = content.split(u'\n', Qt::SkipEmptyParts);
            r.text = r.files.join(u'\n');
            break;
        default:
            ++history.skipped;
            continue;
        }
        if (!r.validate()) {
            ++history.skipped;
            continue;
        }
        if (auto result = consume(std::move(r)); !result)
            return std::unexpected(result.error());
        ++history.imported;
    }
    if (q.lastError().isValid())
        return std::unexpected(q.lastError().text());
    return history;
}
} // namespace mt
