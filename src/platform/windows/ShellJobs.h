#pragma once
#include "modules/launcher/LauncherCatalog.h"
#include <QHash>
#include <QCache>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <functional>
class QProcess;
namespace mt {
// Shell extensions run outside the GUI process with a per-request timeout.
// Icons reuse up to three lightweight helpers; explicit actions retain their own process.
class ShellJobs final : public QObject {
    Q_OBJECT
  public:
    using IconCallback = std::function<void(QImage)>;
    explicit ShellJobs(QObject* parent = nullptr);
    ~ShellJobs() override;
    void icon(const QString& id, const QString& source, const QString& root, IconCallback callback);
    void prioritizeIcons(const QStringList& ids);
    void reveal(const QString& target, const QString& root,
                std::function<void(std::expected<void, QString>)> callback);
    void launch(const LaunchItem& item, const QString& root,
                std::function<void(std::expected<void, QString>)> callback,
                std::function<bool()> permitted = {});
    void importPaths(const QStringList& paths, const QString& root,
                     std::function<void(std::expected<QList<LaunchItem>, QString>)> callback);
    void shutdown();
    bool hasActiveActions() const { return !activeActions_.isEmpty(); }
    void cancelQueuedActions();

  private:
    struct Request {
        QJsonObject payload;
        int timeout = 3000;
        std::function<void(std::expected<QJsonObject, QString>)> callback;
        std::function<bool()> permitted;
    };
    void enqueue(Request request);
    void pump();
    void schedulePump();
    QQueue<Request> queue_;
    QSet<QProcess*> active_;
    QSet<QProcess*> activeActions_;
    QList<QProcess*> idleIcons_;
    QTimer idleTimer_;
    QCache<QString, QImage> iconCache_{256};
    QHash<QString, QList<IconCallback>> iconWaiters_;
    QSet<QString> priorityIcons_;
    bool pumpScheduled_ = false;
    bool stopping_ = false;
};
} // namespace mt
