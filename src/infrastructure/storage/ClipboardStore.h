#pragma once
#include "modules/clipboard/ClipboardRecord.h"
#include <QSqlDatabase>
class QSqlQuery;

namespace mt {
// Construct, use and destroy on one worker thread. Never stores GUI resources.
class ClipboardStore final {
  public:
    explicit ClipboardStore(const QString& path);
    ~ClipboardStore();
    ClipboardStore(const ClipboardStore&) = delete;
    ClipboardStore& operator=(const ClipboardStore&) = delete;
    std::expected<void, QString> open();
    // storageLimit bounds ordinary records; favorites and pinned are never pruned.
    std::expected<void, QString> capture(ClipboardRecord record, int storageLimit);
    // All-or-nothing: either every record is merged or the history is unchanged.
    std::expected<int, QString> import(ClipboardRecords records, int storageLimit);
    // Reads/encodes one payload at a time; producer errors and exceptions roll
    // back every inserted row. The source must not retain decoded payloads.
    std::expected<int, QString> importStream(const ClipProducer& produce, int storageLimit);
    // displayLimit bounds ordinary records; favorites and pinned are always listed.
    std::expected<ClipboardRecords, QString> records(const QString& query = {}, int filter = -1,
                                                     int displayLimit = 500) const;
    std::expected<void, QString> setFlags(const QString& id, bool favorite, bool pinned);
    // UI actions are operations, not absolute values from a stale list snapshot.
    std::expected<void, QString> toggleFlag(const QString& id, ClipFlag flag);
    std::expected<void, QString> remove(const QString& id);
    // Moves a confirmed record to the top of the history.
    std::expected<void, QString> touch(const QString& id);
    std::expected<void, QString> clear();
    std::expected<ClipboardRecord, QString> record(const QString& id) const;

  private:
    static std::expected<void, QString> insert(QSqlQuery& query, const ClipboardRecord& record,
                                               const char* sql);
    static std::expected<void, QString> prune(QSqlQuery& query, int storageLimit);
    QString path_;
    QString name_;
    QSqlDatabase db_;
};
} // namespace mt
