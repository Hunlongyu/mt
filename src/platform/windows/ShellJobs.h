#pragma once
#include "modules/launcher/LauncherCatalog.h"
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <functional>
class QProcess;
namespace mt {
// Shell extensions are third-party code and may block indefinitely. Each request runs in a
// bounded, non-elevated helper invocation of the same executable, never in the GUI process.
class ShellJobs final : public QObject {
    Q_OBJECT
  public:
    using IconCallback = std::function<void(QImage)>;
    explicit ShellJobs(QObject* parent = nullptr);
    ~ShellJobs() override;
    void icon(const QString& id, const QString& source, const QString& root, IconCallback callback);
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
    QQueue<Request> queue_;
    QSet<QProcess*> active_;
    QSet<QProcess*> activeActions_;
    QHash<QString, QImage> iconCache_;
    QHash<QString, QList<IconCallback>> iconWaiters_;
    bool stopping_ = false;
};
} // namespace mt
