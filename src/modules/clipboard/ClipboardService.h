#pragma once
#include "ClipboardRecord.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace mt {
class ClipboardWorker;
class ClipboardStore;
class ClipboardService final : public QObject {
    Q_OBJECT
  public:
    explicit ClipboardService(const QString& path, QObject* parent = nullptr);
    ~ClipboardService() override;
    void start(bool observeSystem);
    // storageLimit bounds what is kept, displayLimit what the panel lists.
    void configure(bool enabled, bool paused, int storageLimit, int displayLimit);
    void refresh(const QString& query = {}, int filter = -1);
    void capture(ClipboardRecord record);
    void toggleFlag(const QString& id, ClipFlag flag);
    void remove(const QString& id);
    void touch(const QString& id);
    void clear(std::function<void(bool)> done);
    // Reads a Hopy data folder on the storage thread and merges it in one transaction.
    void importHopy(const QString& folder, std::function<void(int imported, int skipped)> done);
    // Returns whether the system clipboard now holds this write (it can be locked
    // by another program); the caller decides whether to retry.
    bool write(const ClipboardRecord& record, bool plainText);
    void retrieve(const QString& id, std::function<void(ClipboardRecord)> continuation);
  Q_SIGNALS:
    void ready();
    void recordsChanged(mt::ClipboardRecords records);
    void failed(const QString& error);
    void diagnostic(const QString& message);

  private:
    friend class ClipboardIntegrationTests;
    void snapshot();
    void snapshotAttempt(quint64 generation, quint32 sequence, int attempt);
    void mutate(std::function<std::expected<void, QString>(ClipboardStore&)> change);
    void requestCurrent();
    void dispatchQuery();
    QString path_;
    QThread thread_;
    ClipboardWorker* worker_ = nullptr;
    QString query_;
    int filter_ = -1;
    std::atomic<quint64> queryGeneration_{0};
    QTimer queryTimer_;
    QString ownMarker_;
    // Obtain a fresh view per attempt; isolated tests supply delayed MIME data.
    std::function<const QMimeData*()> clipboardMimeData_;
    std::function<quint32()> clipboardSequence_;
    quint64 captureGeneration_ = 0;
    bool enabled_ = true;
    bool paused_ = false;
    bool observing_ = false;
    int storageLimit_ = 1000;
    int displayLimit_ = 500;
    int pending_ = 0;
};
} // namespace mt
