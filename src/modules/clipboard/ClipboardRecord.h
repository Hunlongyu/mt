#pragma once
#include <QDateTime>
#include <QImage>
#include <QMap>
#include <QMimeData>
#include <QStringList>
#include <expected>
#include <functional>
#include <memory>

namespace mt {
enum class ClipKind { Text, Html, Image, Files, Formats };
enum class ClipFlag { Favorite, Pinned };
struct ClipboardRecord {
    static constexpr qsizetype maxText = 1'000'000;
    static constexpr qsizetype maxHtml = 4'000'000;
    static constexpr qsizetype maxFileCharacters = 4'000'000;
    static constexpr int maxFiles = 1000;
    static constexpr int maxImageSide = 16384;
    static constexpr qint64 maxImageBytes = 64 * 1024 * 1024;
    static constexpr int maxFormats = 64;
    static constexpr qsizetype maxFormatBytes = 16 * 1024 * 1024;
    QString id;
    ClipKind kind = ClipKind::Text;
    QString text;
    QString html;
    QStringList files;
    QByteArray image;
    QByteArray thumbnail;
    QMap<QString, QByteArray> formats;
    bool summaryOnly = false;
    QImage pendingImage;
    QString source; // Kept for the development schema v1 only; not captured or shown.
    QDateTime captured = QDateTime::currentDateTimeUtc();
    bool favorite = false;
    bool pinned = false;
    bool sensitive = false;
    QString summary() const;
    QByteArray encodeFormats() const;
    std::expected<void, QString> decodeFormats(const QByteArray& data);
    static bool persistentFormat(const QString& name);
    std::expected<void, QString> validate() const;
    void finalize(); // worker thread: image encoding and content hash
    std::unique_ptr<QMimeData> mime(bool plainText = false) const;
    static std::expected<ClipboardRecord, QString> snapshot(const QMimeData& mime);
};
using ClipboardRecords = QList<ClipboardRecord>;
using ClipConsumer = std::function<std::expected<void, QString>(ClipboardRecord)>;
using ClipProducer = std::function<std::expected<void, QString>(const ClipConsumer&)>;
} // namespace mt
Q_DECLARE_METATYPE(mt::ClipboardRecord)
Q_DECLARE_METATYPE(mt::ClipboardRecords)
Q_DECLARE_METATYPE(mt::ClipFlag)
