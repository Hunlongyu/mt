#include "ClipboardRecord.h"
#include "infrastructure/i18n/I18n.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
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
    return text;
}
std::expected<ClipboardRecord, QString> ClipboardRecord::snapshot(const QMimeData& m) {
    ClipboardRecord r;
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
        if (!r.files.isEmpty()) {
            r.kind = ClipKind::Files;
            r.text = r.files.join(u'\n');
            return r;
        }
    }
    const auto html = m.hasHtml() ? m.html() : QString{};
    // Browser image copies may contain HTML with only an <img> element. A
    // bitmap must win when there is no text format; keep HTML-only fallback.
    if (!html.isEmpty() && (m.hasText() || !m.hasImage())) {
        r.kind = ClipKind::Html;
        r.html = html;
        r.text = m.text();
        if (r.html.size() > 4'000'000 || r.text.size() > 1'000'000)
            return std::unexpected(tx(u"富文本长度超限"));
        if (r.text.isEmpty()) {
            QTextDocument document;
            document.setHtml(r.html);
            r.text = document.toPlainText();
        }
    } else if (m.hasImage()) {
        r.kind = ClipKind::Image;
        r.pendingImage = qvariant_cast<QImage>(m.imageData());
        if (r.pendingImage.isNull())
            return std::unexpected(tx(u"图片格式不可读取"));
        if (r.pendingImage.width() > 16384 || r.pendingImage.height() > 16384 ||
            r.pendingImage.sizeInBytes() > 64 * 1024 * 1024)
            return std::unexpected(tx(u"图片尺寸或内存超过限制"));
    } else if (m.hasText() && !m.text().isEmpty()) {
        r.kind = ClipKind::Text;
        r.text = m.text();
        if (r.text.size() > 1'000'000)
            return std::unexpected(tx(u"文本长度超限"));
    } else
        return std::unexpected(QStringLiteral("unsupported"));
    if (auto valid = r.validate(); !valid)
        return std::unexpected(valid.error());
    return r;
}
std::expected<void, QString> ClipboardRecord::validate() const {
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
        if (pendingImage.width() > maxImageSide || pendingImage.height() > maxImageSide ||
            pendingImage.sizeInBytes() > maxImageBytes)
            return std::unexpected(tx(u"图片尺寸或内存超过限制"));
    } else {
        if (text.isEmpty() && html.isEmpty())
            return std::unexpected(QStringLiteral("unsupported"));
        if (text.size() > maxText || html.size() > maxHtml)
            return std::unexpected(kind == ClipKind::Html ? tx(u"富文本长度超限")
                                                         : tx(u"文本长度超限"));
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
    id = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}
std::unique_ptr<QMimeData> ClipboardRecord::mime(bool plainText) const {
    if (summaryOnly)
        throw std::logic_error("Load the full clipboard payload before writeback");
    auto m = std::make_unique<QMimeData>();
    if (sensitive)
        m->setData(QStringLiteral(
                       "application/"
                       "x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                   QByteArray(4, '\0'));
    if (plainText && kind != ClipKind::Image)
        m->setText(summary());
    else if (kind == ClipKind::Files) {
        QList<QUrl> urls;
        for (const auto& f : files)
            urls.append(QUrl::fromLocalFile(f));
        m->setUrls(urls);
        m->setText(text);
    } else if (kind == ClipKind::Image)
        m->setImageData(QImage::fromData(image));
    else {
        m->setText(text);
        if (kind == ClipKind::Html)
            m->setHtml(html);
    }
    return m;
}
} // namespace mt
