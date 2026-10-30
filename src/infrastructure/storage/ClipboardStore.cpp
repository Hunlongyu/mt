#include "ClipboardStore.h"
#include "infrastructure/i18n/I18n.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QScopeGuard>
#include <QTimeZone>
#include <QUuid>

namespace mt {
ClipboardStore::ClipboardStore(const QString& path)
    : path_(path), name_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
ClipboardStore::~ClipboardStore() {
    if (db_.isValid()) {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
}
std::expected<void, QString> ClipboardStore::open() {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        return std::unexpected(tx(u"无法创建历史目录"));
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
    db_.setDatabaseName(path_);
    db_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!db_.open())
        return std::unexpected(db_.lastError().text());
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next())
        return std::unexpected(q.lastError().text());
    const int version = q.value(0).toInt();
    if (version < 0 || version > 2)
        return std::unexpected(tx(u"不支持此历史数据库版本，已保留原文件"));
    if (version == 0) {
        if (!q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name NOT "
                                   "LIKE 'sqlite_%' LIMIT 1")))
            return std::unexpected(q.lastError().text());
        if (q.next())
            return std::unexpected(tx(u"历史数据库缺少版本标记，已保留原文件，未初始化或升级"));
    }
    if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
        return std::unexpected(q.lastError().text());
    q.finish();
    if (version == 1) {
        // SQLite includes committed WAL content in this consistent backup.
        q.prepare(QStringLiteral("VACUUM INTO ?"));
        q.addBindValue(path_ + QStringLiteral(".schema1-") +
                       QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak"));
        if (!q.exec())
            return std::unexpected(q.lastError().text());
        q.finish();
    }
    if (!db_.transaction())
        return std::unexpected(db_.lastError().text());
    auto rollback = qScopeGuard([this] { db_.rollback(); });
    if (version == 1 &&
        !q.exec(QStringLiteral("ALTER TABLE clips ADD COLUMN formats BLOB NOT NULL DEFAULT '{}'")))
        return std::unexpected(q.lastError().text());
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS clips (id TEXT PRIMARY KEY, kind INTEGER NOT NULL, text "
            "TEXT NOT NULL, html TEXT NOT NULL, files TEXT NOT NULL, image BLOB, source TEXT NOT "
            "NULL, captured INTEGER NOT NULL, favorite INTEGER NOT NULL DEFAULT 0, pinned INTEGER "
            "NOT NULL DEFAULT 0, sensitive INTEGER NOT NULL DEFAULT 0, thumbnail BLOB, "
            "formats BLOB NOT NULL DEFAULT '{}')")) ||
        !q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS clips_order ON clips(pinned DESC, captured DESC)")) ||
        !q.exec(QStringLiteral("PRAGMA user_version=2")) || !db_.commit())
        return std::unexpected(q.lastError().text());
    rollback.dismiss();
    return {};
}
namespace {
// New captures move an existing duplicate to the top; imports keep whichever
// copy is newer and never clear a flag the user set on either side.
constexpr auto upsertCapture =
    "INSERT INTO clips(id,kind,text,html,files,image,source,captured,favorite,pinned,sensitive,"
    "thumbnail,formats) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
    "captured=excluded.captured,source=excluded.source,"
    "sensitive=MAX(clips.sensitive,excluded.sensitive)";
constexpr auto upsertImport =
    "INSERT INTO clips(id,kind,text,html,files,image,source,captured,favorite,pinned,sensitive,"
    "thumbnail,formats) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
    "captured=MAX(clips.captured,excluded.captured),favorite=MAX(clips.favorite,excluded.favorite),"
    "pinned=MAX(clips.pinned,excluded.pinned),sensitive=MAX(clips.sensitive,excluded.sensitive)";
} // namespace
std::expected<void, QString> ClipboardStore::capture(ClipboardRecord r, int limit) {
    if (limit < 1)
        return std::unexpected(tx(u"历史上限无效"));
    if (auto valid = r.validate(); !valid)
        return valid;
    r.finalize();
    if (!db_.transaction())
        return std::unexpected(db_.lastError().text());
    QSqlQuery q(db_);
    if (auto inserted = insert(q, r, upsertCapture); !inserted) {
        db_.rollback();
        return inserted;
    }
    if (auto pruned = prune(q, limit); !pruned || !db_.commit()) {
        db_.rollback();
        return pruned ? std::unexpected(db_.lastError().text()) : pruned;
    }
    return {};
}
std::expected<void, QString> ClipboardStore::insert(QSqlQuery& q, const ClipboardRecord& r,
                                                    const char* sql) {
    q.prepare(QString::fromLatin1(sql));
    q.addBindValue(r.id);
    q.addBindValue(static_cast<int>(r.kind));
    q.addBindValue(r.text.isNull() ? QStringLiteral("") : r.text);
    q.addBindValue(r.html.isNull() ? QStringLiteral("") : r.html);
    q.addBindValue(QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(r.files)).toJson(QJsonDocument::Compact)));
    q.addBindValue(r.image);
    q.addBindValue(r.source.isNull() ? QStringLiteral("") : r.source);
    q.addBindValue(r.captured.toMSecsSinceEpoch());
    q.addBindValue(r.favorite);
    q.addBindValue(r.pinned);
    q.addBindValue(r.sensitive);
    q.addBindValue(r.thumbnail);
    q.addBindValue(r.encodeFormats());
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<void, QString> ClipboardStore::prune(QSqlQuery& q, int limit) {
    q.prepare(QStringLiteral("DELETE FROM clips WHERE id IN (SELECT id FROM clips WHERE favorite=0 "
                             "AND pinned=0 ORDER BY captured DESC,id DESC LIMIT -1 OFFSET ?)"));
    q.addBindValue(limit);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<int, QString> ClipboardStore::import(ClipboardRecords records, int limit) {
    return importStream([&records](const ClipConsumer& consume) -> std::expected<void, QString> {
        for (auto& record : records)
            if (auto result = consume(std::move(record)); !result)
                return result;
        return {};
    }, limit);
}
std::expected<int, QString> ClipboardStore::importStream(const ClipProducer& produce, int limit) {
    if (limit < 1)
        return std::unexpected(tx(u"历史上限无效"));
    if (!db_.transaction())
        return std::unexpected(db_.lastError().text());
    auto rollback = qScopeGuard([this] { db_.rollback(); });
    QSqlQuery q(db_);
    int count = 0;
    const auto read = produce([&](ClipboardRecord r) -> std::expected<void, QString> {
        if (auto valid = r.validate(); !valid)
            return valid;
        r.finalize();
        if (auto inserted = insert(q, r, upsertImport); !inserted)
            return inserted;
        ++count;
        return {};
    });
    if (!read)
        return std::unexpected(read.error());
    if (auto pruned = prune(q, limit); !pruned || !db_.commit())
        return std::unexpected(pruned ? db_.lastError().text() : pruned.error());
    rollback.dismiss();
    return count;
}
std::expected<void, QString> ClipboardStore::touch(const QString& id) {
    QSqlQuery q(db_);
    // Windows clocks can repeat a millisecond (or move backwards). A confirmed
    // record must sort above existing ordinary history even in that case.
    q.prepare(QStringLiteral("UPDATE clips SET captured=MAX(?, "
                             "COALESCE((SELECT MAX(captured) FROM clips), 0)+1) WHERE id=?"));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<void, QString> ClipboardStore::clear() {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("DELETE FROM clips")))
        return std::unexpected(q.lastError().text());
    // Reclaim the space of large images; a failed VACUUM leaves valid, empty data.
    (void)q.exec(QStringLiteral("VACUUM"));
    return {};
}
std::expected<ClipboardRecords, QString> ClipboardStore::records(const QString& query, int filter,
                                                                 int limit) const {
    QSqlQuery q(db_);
    QString where = QStringLiteral(" WHERE 1=1");
    if (filter == 0)
        where += QStringLiteral(" AND kind IN (0,1,4)");
    if (filter == 1)
        where += QStringLiteral(" AND kind=2");
    if (filter == 2)
        where += QStringLiteral(" AND kind=3");
    if (filter == 3)
        where += QStringLiteral(" AND favorite=1");
    if (!query.isEmpty())
        where += QStringLiteral(" AND (text LIKE ? ESCAPE '\\' OR files LIKE ? ESCAPE '\\')");
    // Favorites and pinned records are always listed; the display limit applies
    // to ordinary history only, exactly like the storage limit.
    const QString sql =
        QStringLiteral("SELECT id,kind,substr(text,1,4096),'','[]',thumbnail,source,captured,"
                       "favorite,pinned,sensitive FROM clips") +
        where +
        QStringLiteral(" AND (favorite=1 OR pinned=1 OR id IN (SELECT id FROM clips") + where +
        QStringLiteral(" AND favorite=0 AND pinned=0 ORDER BY captured DESC,id DESC LIMIT ?))"
                       " ORDER BY pinned DESC,captured DESC,id DESC");
    q.prepare(sql);
    QString pattern;
    if (!query.isEmpty()) {
        auto escaped = query;
        escaped.replace(u'\\', QStringLiteral("\\\\"))
            .replace(u'%', QStringLiteral("\\%"))
            .replace(u'_', QStringLiteral("\\_"));
        pattern = u'%' + escaped + u'%';
    }
    for (int pass = 0; pass < 2; ++pass)
        if (!pattern.isEmpty()) {
            q.addBindValue(pattern);
            q.addBindValue(pattern);
        }
    q.addBindValue(qBound(1, limit, 100000));
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    ClipboardRecords records;
    while (q.next()) {
        ClipboardRecord r;
        r.id = q.value(0).toString();
        r.kind = static_cast<ClipKind>(q.value(1).toInt());
        r.text = q.value(2).toString();
        r.html = q.value(3).toString();
        for (const auto& v : QJsonDocument::fromJson(q.value(4).toByteArray()).array())
            r.files.append(v.toString());
        r.image = q.value(5).toByteArray();
        r.source = q.value(6).toString();
        r.captured = QDateTime::fromMSecsSinceEpoch(q.value(7).toLongLong(), QTimeZone::UTC);
        r.favorite = q.value(8).toBool();
        r.pinned = q.value(9).toBool();
        r.sensitive = q.value(10).toBool();
        r.summaryOnly = true;
        records.append(std::move(r));
    }
    return records;
}
std::expected<void, QString> ClipboardStore::setFlags(const QString& id, bool favorite,
                                                      bool pinned) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE clips SET favorite=?,pinned=? WHERE id=?"));
    q.addBindValue(favorite);
    q.addBindValue(pinned);
    q.addBindValue(id);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<void, QString> ClipboardStore::toggleFlag(const QString& id, ClipFlag flag) {
    QSqlQuery q(db_);
    q.prepare(flag == ClipFlag::Favorite
                  ? QStringLiteral("UPDATE clips SET favorite=1-favorite WHERE id=?")
                  : QStringLiteral("UPDATE clips SET pinned=1-pinned WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<void, QString> ClipboardStore::remove(const QString& id) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM clips WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    return {};
}
std::expected<ClipboardRecord, QString> ClipboardStore::record(const QString& id) const {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT id,kind,text,html,files,image,source,captured,favorite,pinned,sensitive,formats "
        "FROM clips "
        "WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec())
        return std::unexpected(q.lastError().text());
    if (!q.next())
        return std::unexpected(tx(u"记录已不存在"));
    ClipboardRecord r;
    r.id = q.value(0).toString();
    r.kind = static_cast<ClipKind>(q.value(1).toInt());
    r.text = q.value(2).toString();
    r.html = q.value(3).toString();
    for (const auto& value : QJsonDocument::fromJson(q.value(4).toByteArray()).array())
        r.files.append(value.toString());
    r.image = q.value(5).toByteArray();
    r.source = q.value(6).toString();
    r.captured = QDateTime::fromMSecsSinceEpoch(q.value(7).toLongLong(), QTimeZone::UTC);
    r.favorite = q.value(8).toBool();
    r.pinned = q.value(9).toBool();
    r.sensitive = q.value(10).toBool();
    if (auto formats = r.decodeFormats(q.value(11).toByteArray()); !formats)
        return std::unexpected(formats.error());
    if (auto valid = r.validate(); !valid)
        return std::unexpected(valid.error());
    return r;
}
} // namespace mt
