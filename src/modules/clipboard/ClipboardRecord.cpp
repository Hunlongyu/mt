#include "ClipboardRecord.h"
#include "infrastructure/i18n/I18n.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>
#include <stdexcept>

namespace mt {
QString ClipboardRecord::summary() const {
    if (kind == ClipKind::Files && !files.isEmpty())
        return files.join(u'\n');
    if (kind == ClipKind::Image)
        return tx(u"图片");
    if (kind == ClipKind::Formats)
        return tx(u"富文本或应用专用内容");
    return text;
}
bool ClipboardRecord::persistentFormat(const QString& name) {
    const auto key = name.toLower();
    if (key.isEmpty() || key.size() > 512 || key.contains(QChar::Null))
        return false;
    for (const auto c : key)
        if (c.unicode() < 32)
            return false;
    // Standard representations are stored in typed fields. Process-local OLE
    // identities, transfer side effects and monitor markers must not be replayed.
    return key != QStringLiteral("text/plain") && key != QStringLiteral("text/html") &&
           key != QStringLiteral("text/uri-list") &&
           key != QStringLiteral("application/x-qt-image") &&
           !key.startsWith(QStringLiteral("application/x-mt-")) &&
           !key.contains(QStringLiteral("excludeclipboardcontentfrommonitorprocessing")) &&
           !key.contains(QStringLiteral("canincludeinclipboardhistory")) &&
           !key.contains(QStringLiteral("canuploadtocloudclipboard")) &&
           !key.contains(QStringLiteral("ole private data")) &&
           !key.contains(QStringLiteral("dataobject")) &&
           !key.contains(QStringLiteral("ownerlink")) &&
           !key.contains(QStringLiteral("objectlink")) &&
           !key.contains(QStringLiteral("link source")) &&
           !key.contains(QStringLiteral("dropeffect")) &&
           !key.contains(QStringLiteral("paste succeeded"));
}
QByteArray ClipboardRecord::encodeFormats() const {
    QJsonObject object;
    for (auto it = formats.cbegin(); it != formats.cend(); ++it)
        object.insert(it.key(), QString::fromLatin1(it.value().toBase64()));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
std::expected<void, QString> ClipboardRecord::decodeFormats(const QByteArray& data) {
    if (data.size() > maxFormatBytes * 2)
        return std::unexpected(tx(u"剪贴板格式数据超限"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().size() > maxFormats)
        return std::unexpected(tx(u"剪贴板格式数据损坏，原记录已保留"));
    QMap<QString, QByteArray> restored;
    qsizetype size = 0;
    const auto object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!it.value().isString() || !persistentFormat(it.key()))
            return std::unexpected(tx(u"剪贴板格式数据损坏，原记录已保留"));
        const auto decoded = QByteArray::fromBase64Encoding(
            it.value().toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || (size += decoded.decoded.size()) > maxFormatBytes)
            return std::unexpected(tx(u"剪贴板格式数据损坏或超限，原记录已保留"));
        restored.insert(it.key(), decoded.decoded);
    }
    formats = std::move(restored);
    return {};
}
std::expected<ClipboardRecord, QString> ClipboardRecord::snapshot(const QMimeData& m) {
    ClipboardRecord r;
    qsizetype bytes = 0;
    const auto names = m.formats();
    if (names.size() > maxFormats)
        return std::unexpected(tx(u"剪贴板格式数量超限"));
    for (const auto& name : names) {
        if (!persistentFormat(name))
            continue;
        const auto data = m.data(name);
        if ((bytes += data.size()) > maxFormatBytes)
            return std::unexpected(tx(u"剪贴板附加格式超过 16 MiB，未记录"));
        r.formats.insert(name, data);
    }
    if (m.hasUrls()) {
        qsizetype length = 0;
        for (const auto& url : m.urls()) {
            if (!url.isLocalFile())
                continue;
            r.files.append(url.toLocalFile());
            length += r.files.last().size();
            if (r.files.size() > maxFiles)
                return std::unexpected(tx(u"文件列表超过 1000 项"));
            if (length > maxFileCharacters)
                return std::unexpected(tx(u"文件路径总长度超限"));
        }
    }
    r.text = m.hasText() ? m.text() : QString{};
    r.html = m.hasHtml() ? m.html() : QString{};
    if (m.hasImage()) {
        r.pendingImage = qvariant_cast<QImage>(m.imageData());
        if (r.pendingImage.isNull())
            return std::unexpected(tx(u"图片格式不可读取"));
    }
    // Keep all parallel representations; kind only selects the list/preview UI.
    if (!r.files.isEmpty()) {
        r.kind = ClipKind::Files;
        if (r.text.isEmpty())
            r.text = r.files.join(u'\n');
    }
    // Browser image copies may contain HTML with only an <img> element. A
    // bitmap must win when there is no text format; keep HTML-only fallback.
    else if (!r.html.isEmpty() && (m.hasText() || !m.hasImage())) {
        r.kind = ClipKind::Html;
        if (r.text.isEmpty()) {
            QTextDocument document;
            document.setHtml(r.html);
            r.text = document.toPlainText();
        }
    } else if (m.hasImage()) {
        r.kind = ClipKind::Image;
    } else if (!r.text.isEmpty()) {
        r.kind = ClipKind::Text;
    } else if (!r.formats.isEmpty())
        r.kind = ClipKind::Formats;
    else
        return std::unexpected(QStringLiteral("unsupported"));
    if (auto valid = r.validate(); !valid)
        return std::unexpected(valid.error());
    return r;
}
std::expected<void, QString> ClipboardRecord::validate() const {
    if (text.size() > maxText || html.size() > maxHtml)
        return std::unexpected(tx(u"剪贴板文本或富文本长度超限"));
    if (pendingImage.width() > maxImageSide || pendingImage.height() > maxImageSide ||
        pendingImage.sizeInBytes() > maxImageBytes)
        return std::unexpected(tx(u"图片尺寸或内存超过限制"));
    qsizetype bytes = 0;
    if (formats.size() > maxFormats)
        return std::unexpected(tx(u"剪贴板格式数量超限"));
    for (auto it = formats.cbegin(); it != formats.cend(); ++it)
        if (!persistentFormat(it.key()) || (bytes += it.value().size()) > maxFormatBytes)
            return std::unexpected(tx(u"剪贴板格式无效或超限"));
    if (kind == ClipKind::Formats)
        return formats.isEmpty() ? std::unexpected(tx(u"剪贴板格式数据为空"))
                                 : std::expected<void, QString>{};
    if (kind < ClipKind::Text || kind > ClipKind::Formats)
        return std::unexpected(tx(u"未知剪贴板类型"));
    if (kind == ClipKind::Files) {
        if (files.isEmpty())
            return std::unexpected(QStringLiteral("unsupported"));
        if (files.size() > maxFiles)
            return std::unexpected(tx(u"文件列表超过 1000 项"));
        qsizetype length = 0;
        for (const auto& file : files)
            length += file.size();
        if (length > maxFileCharacters)
            return std::unexpected(tx(u"文件路径总长度超限"));
    } else if (kind == ClipKind::Image) {
        if (pendingImage.isNull() && image.isEmpty())
            return std::unexpected(tx(u"图片格式不可读取"));
    } else {
        if (text.isEmpty() && html.isEmpty())
            return std::unexpected(QStringLiteral("unsupported"));
    }
    return {};
}
void ClipboardRecord::finalize() {
    if (summaryOnly)
        throw std::logic_error("A summary must never replace a full clipboard payload");
    if (!pendingImage.isNull()) {
        QBuffer previewBuffer(&thumbnail);
        previewBuffer.open(QIODevice::WriteOnly);
        pendingImage.scaled(160, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .save(&previewBuffer, "PNG");
        QBuffer buffer(&image);
        buffer.open(QIODevice::WriteOnly);
        if (!pendingImage.save(&buffer, "PNG"))
            throw std::runtime_error("Image encoding failed");
        pendingImage = {};
    }
    QByteArray content;
    QDataStream stream(&content, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_8);
    stream << static_cast<qint32>(kind) << text << html << files << image;
    if (!formats.isEmpty())
        stream << formats;
    id = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}
std::unique_ptr<QMimeData> ClipboardRecord::mime(bool plainText) const {
    if (summaryOnly)
        throw std::logic_error("Load the full clipboard payload before writeback");
    if (plainText && kind == ClipKind::Formats)
        throw std::logic_error("This clipboard record has no plain-text representation");
    auto m = std::make_unique<QMimeData>();
    if (sensitive)
        m->setData(QStringLiteral(
                       "application/"
                       "x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                   QByteArray(4, '\0'));
    if (plainText) {
        if (kind == ClipKind::Image)
            m->setImageData(QImage::fromData(image));
        else
            m->setText(summary());
    } else {
        if (!text.isEmpty())
            m->setText(text);
        if (!html.isEmpty())
            m->setHtml(html);
        if (!image.isEmpty())
            m->setImageData(QImage::fromData(image));
        if (!files.isEmpty()) {
            QList<QUrl> urls;
            for (const auto& f : files)
                urls.append(QUrl::fromLocalFile(f));
            m->setUrls(urls);
        }
    }
    if (!plainText)
        for (auto it = formats.cbegin(); it != formats.cend(); ++it)
            m->setData(it.key(), it.value());
    return m;
}
} // namespace mt
