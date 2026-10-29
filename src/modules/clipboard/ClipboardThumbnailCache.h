#pragma once
#include "ClipboardRecord.h"
#include <QCache>
#include <QPixmap>
#include <QSet>
#include <algorithm>
#include <climits>

namespace mt {
// Owned and accessed by the list delegate on the GUI thread. Records carry PNG
// thumbnails; keep their decoded, device-scaled pixmaps across repaints/flag updates.
class ClipboardThumbnailCache final {
  public:
    explicit ClipboardThumbnailCache(int budgetKiB = 8 * 1024) : entries_(budgetKiB) {}
    QPixmap pixmap(const ClipboardRecord& record, QSize bounds, qreal dpr) {
        if (bounds.isEmpty() || dpr <= 0)
            return {};
        if (const auto* entry = entries_.object(record.id);
            entry && entry->png == record.image && entry->bounds == bounds && entry->dpr == dpr)
            return entry->pixmap;
        const auto image = QImage::fromData(record.image).scaled(
            QSize(qRound(bounds.width() * dpr), qRound(bounds.height() * dpr)),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto result = QPixmap::fromImage(image);
        result.setDevicePixelRatio(dpr);
        // Charge for the native pixels and retained encoded bytes, with a minimum
        // cost for empty/invalid images so negative cache entries are bounded too.
        const auto bytes = qint64(result.width()) * result.height() * 4 + record.image.size();
        const int cost = static_cast<int>((std::min)(qint64(INT_MAX),
                                                   (std::max)(qint64(1), (bytes + 1023) / 1024)));
        entries_.insert(record.id, new Entry{record.image, bounds, dpr, result}, cost);
        return result;
    }
    void retain(const ClipboardRecords& records) {
        QSet<QString> ids;
        for (const auto& record : records)
            if (record.kind == ClipKind::Image)
                ids.insert(record.id);
        for (const auto& id : entries_.keys())
            if (!ids.contains(id))
                entries_.remove(id);
    }
    qsizetype costKiB() const { return entries_.totalCost(); }
    qsizetype budgetKiB() const { return entries_.maxCost(); }

  private:
    struct Entry {
        QByteArray png;
        QSize bounds;
        qreal dpr;
        QPixmap pixmap;
    };
    QCache<QString, Entry> entries_;
};
} // namespace mt
