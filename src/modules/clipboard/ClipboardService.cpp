#include "ClipboardService.h"
#include "infrastructure/i18n/I18n.h"
#include "infrastructure/storage/ClipboardStore.h"
#include "infrastructure/storage/HopyImport.h"
#include "platform/windows/DesktopServices.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QUuid>

namespace mt {
class ClipboardWorker final : public QObject {
  public:
    std::unique_ptr<ClipboardStore> store;
    bool ready = false;
};
ClipboardService::ClipboardService(const QString& path, QObject* parent)
    : QObject(parent), path_(path), ownMarker_(QUuid::createUuid().toString()) {
    queryTimer_.setSingleShot(true);
    queryTimer_.setInterval(40);
    connect(&queryTimer_, &QTimer::timeout, this, &ClipboardService::dispatchQuery);
}
ClipboardService::~ClipboardService() {
    observing_ = false;
    queryTimer_.stop();
    ++queryGeneration_;
    if (thread_.isRunning()) {
        // All previous writes complete in FIFO order, then the connection is destroyed on its
        // owning thread.
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_] {
                worker->store.reset();
                worker->ready = false;
            },
            Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
}
void ClipboardService::start(bool observeSystem) {
    if (worker_)
        return;
    observing_ = observeSystem;
    worker_ = new ClipboardWorker;
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("clipboard-storage"));
    thread_.start();
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, path = path_] {
            worker->store = std::make_unique<ClipboardStore>(path);
            const auto opened = worker->store->open();
            worker->ready = opened.has_value();
            QMetaObject::invokeMethod(
                this,
                [this, opened] {
                    if (opened) {
                        Q_EMIT ready();
                        requestCurrent();
                    } else
                        Q_EMIT failed(opened.error());
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (observeSystem)
        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
                &ClipboardService::snapshot);
}
void ClipboardService::configure(bool enabled, bool paused, int storageLimit, int displayLimit) {
    enabled_ = enabled;
    paused_ = paused;
    const bool listChanged = displayLimit != displayLimit_;
    storageLimit_ = storageLimit;
    displayLimit_ = displayLimit;
    if (listChanged)
        requestCurrent();
}
void ClipboardService::snapshot() {
    if (!observing_ || !enabled_ || paused_)
        return;
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return;
    if (QString::fromUtf8(mime->data(QStringLiteral("application/x-mt-writeback"))) ==
        ownMarker_)
        return;
    auto record = ClipboardRecord::snapshot(*mime);
    if (!record) {
        if (record.error() != QStringLiteral("unsupported"))
            Q_EMIT failed(record.error());
        return;
    }
    record->sensitive = DesktopServices::clipboardSensitive();
    capture(std::move(*record));
}
void ClipboardService::capture(ClipboardRecord record) {
    if (!worker_)
        return;
    if (pending_ >= 4) {
        Q_EMIT failed(tx(u"剪贴板保存队列繁忙，本次内容未记录"));
        return;
    }
    ++pending_;
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, record = std::move(record), limit = storageLimit_]() mutable {
            std::expected<void, QString> result;
            try {
                result = worker->ready ? worker->store->capture(std::move(record), limit)
                                       : std::unexpected(tx(u"历史存储不可用"));
            } catch (const std::exception&) {
                result = std::unexpected(tx(u"剪贴板编码或保存失败，已有数据未删除"));
            }
            QMetaObject::invokeMethod(
                this,
                [this, result] {
                    --pending_;
                    if (!result)
                        Q_EMIT failed(result.error());
                    else
                        requestCurrent();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void ClipboardService::refresh(const QString& query, int filter) {
    query_ = query;
    filter_ = filter;
    requestCurrent();
}
void ClipboardService::requestCurrent() {
    if (!worker_)
        return;
    // Invalidate already queued work immediately, then merge a typing burst.
    // Captures go straight to the worker and are never delayed by this timer.
    ++queryGeneration_;
    queryTimer_.start();
}
void ClipboardService::dispatchQuery() {
    const auto generation = queryGeneration_.load();
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, query = query_, filter = filter_, limit = displayLimit_,
         generation] {
            if (generation != queryGeneration_.load() || !worker->ready)
                return;
            auto result = worker->store->records(query, filter, limit);
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result), generation]() mutable {
                    if (generation != queryGeneration_.load())
                        return;
                    if (!result)
                        Q_EMIT failed(result.error());
                    else
                        Q_EMIT recordsChanged(std::move(*result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void ClipboardService::toggleFlag(const QString& id, ClipFlag flag) {
    mutate([=](ClipboardStore& store) { return store.toggleFlag(id, flag); });
}
void ClipboardService::remove(const QString& id) {
    mutate([id](ClipboardStore& store) { return store.remove(id); });
}
void ClipboardService::touch(const QString& id) {
    mutate([id](ClipboardStore& store) { return store.touch(id); });
}
void ClipboardService::clear(std::function<void(bool)> done) {
    if (!worker_)
        return;
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, done = std::move(done)]() mutable {
            auto result = worker->ready ? worker->store->clear()
                                        : std::unexpected(tx(u"历史存储不可用"));
            QMetaObject::invokeMethod(
                this,
                [this, result, done = std::move(done)] {
                    if (!result)
                        Q_EMIT failed(result.error());
                    requestCurrent();
                    done(result.has_value());
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void ClipboardService::importHopy(const QString& folder,
                                  std::function<void(int, int)> done) {
    if (!worker_)
        return;
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, folder, limit = storageLimit_, done = std::move(done)]() mutable {
            std::expected<int, QString> imported = std::unexpected(tx(u"历史存储不可用"));
            int skipped = 0;
            try {
                if (worker->ready) {
                    imported = worker->store->importStream(
                        [&](const ClipConsumer& consume) -> std::expected<void, QString> {
                            auto history = readHopyHistory(folder, consume);
                            if (!history)
                                return std::unexpected(history.error());
                            skipped = history->skipped;
                            return {};
                        }, limit);
                }
            } catch (const std::exception&) {
                imported = std::unexpected(tx(u"导入时图片编码失败，现有历史未改变"));
            }
            QMetaObject::invokeMethod(
                this,
                [this, imported, skipped, done = std::move(done)] {
                    if (!imported) {
                        Q_EMIT failed(imported.error());
                        done(-1, skipped);
                        return;
                    }
                    requestCurrent();
                    done(*imported, skipped);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void ClipboardService::mutate(std::function<std::expected<void, QString>(ClipboardStore&)> change) {
    if (!worker_)
        return;
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, change = std::move(change)] {
            if (!worker->ready)
                return;
            auto result = change(*worker->store);
            QMetaObject::invokeMethod(
                this,
                [this, result] {
                    if (!result)
                        Q_EMIT failed(result.error());
                    else
                        requestCurrent();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
bool ClipboardService::write(const ClipboardRecord& record, bool plain) {
    if (plain && record.kind == ClipKind::Formats) {
        Q_EMIT failed(tx(u"该内容没有纯文本格式，请使用普通粘贴"));
        return false;
    }
    auto data = record.mime(plain);
    data->setData(QStringLiteral("application/x-mt-writeback"), ownMarker_.toUtf8());
    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setMimeData(data.release());
    const auto* current = clipboard->mimeData();
    return current &&
           QString::fromUtf8(current->data(QStringLiteral("application/x-mt-writeback"))) ==
               ownMarker_;
}
void ClipboardService::retrieve(const QString& id,
                                std::function<void(ClipboardRecord)> continuation) {
    if (!worker_)
        return;
    QMetaObject::invokeMethod(
        worker_,
        [this, worker = worker_, id, continuation = std::move(continuation)]() mutable {
            if (!worker->ready)
                return;
            auto result = worker->store->record(id);
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result),
                 continuation = std::move(continuation)]() mutable {
                    if (!result)
                        Q_EMIT failed(result.error());
                    else
                        continuation(std::move(*result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
} // namespace mt
