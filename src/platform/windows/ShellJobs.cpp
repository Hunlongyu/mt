#include "ShellJobs.h"
#include "infrastructure/i18n/I18n.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace mt {
namespace {
QJsonObject itemObject(const LaunchItem& item) {
    return {{QStringLiteral("id"), item.id},
            {QStringLiteral("name"), item.name},
            {QStringLiteral("target"), item.target},
            {QStringLiteral("arguments"), item.arguments},
            {QStringLiteral("workingDirectory"), item.workingDirectory},
            {QStringLiteral("iconPath"), item.iconPath},
            {QStringLiteral("elevated"), item.elevated}};
}
LaunchItem itemValue(const QJsonObject& o) {
    return {o.value(QStringLiteral("id")).toString(),
            o.value(QStringLiteral("name")).toString(),
            o.value(QStringLiteral("target")).toString(),
            o.value(QStringLiteral("arguments")).toString(),
            o.value(QStringLiteral("workingDirectory")).toString(),
            o.value(QStringLiteral("iconPath")).toString(),
            o.value(QStringLiteral("elevated")).toBool()};
}
} // namespace
ShellJobs::ShellJobs(QObject* parent) : QObject(parent) {}
ShellJobs::~ShellJobs() {
    shutdown();
}
void ShellJobs::shutdown() {
    if (stopping_)
        return;
    stopping_ = true;
    queue_.clear();
    iconWaiters_.clear();
    const auto running = active_;
    active_.clear();
    activeActions_.clear();
    for (auto* process : running) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
        delete process;
    }
}
void ShellJobs::cancelQueuedActions() {
    queue_.removeIf([](const Request& request) {
        return request.payload.value(QStringLiteral("operation")).toString() != QStringLiteral("icon");
    });
}
void ShellJobs::enqueue(Request request) {
    if (stopping_)
        return;
    if (request.payload.value(QStringLiteral("operation")).toString() == QStringLiteral("icon"))
        queue_.enqueue(std::move(request));
    else {
        // Actions take precedence over queued decoration work, preserving action order.
        qsizetype position = 0;
        while (position < queue_.size() &&
               queue_[position].payload.value(QStringLiteral("operation")).toString() !=
                   QStringLiteral("icon"))
            ++position;
        queue_.insert(position, std::move(request));
    }
    pump();
}
void ShellJobs::pump() {
    while (!stopping_ && active_.size() < 4 && !queue_.isEmpty()) {
        // Keep one slot available for explicit user actions while slow icon providers run.
        if (active_.size() >= 3 &&
            queue_.head().payload.value(QStringLiteral("operation")).toString() ==
                QStringLiteral("icon"))
            break;
        auto request = queue_.dequeue();
        if (request.permitted && !request.permitted())
            continue;
        auto* process = new QProcess(this);
        active_.insert(process);
        if (request.payload.value(QStringLiteral("operation")).toString() != QStringLiteral("icon"))
            activeActions_.insert(process);
        auto* timer = new QTimer(process);
        timer->setSingleShot(true);
        auto output = std::make_shared<QByteArray>();
        auto complete = std::make_shared<bool>(false);
        auto timedOut = std::make_shared<bool>(false);
        auto finish = [this, process, timer, request, output, complete,
                       timedOut](int code) mutable {
            if (*complete)
                return;
            *complete = true;
            timer->stop();
            active_.remove(process);
            activeActions_.remove(process);
            std::expected<QJsonObject, QString> result =
                std::unexpected(tx(u"Shell 操作失败"));
            if (*timedOut)
                result =
                    std::unexpected(request.payload.value(QStringLiteral("operation")).toString() ==
                                            QStringLiteral("launch")
                                        ? tx(u"启动请求超时，结果未知；未自动重试")
                                        : tx(u"Shell 查询超时"));
            else if (code == 0) {
                QJsonParseError error;
                const auto doc = QJsonDocument::fromJson(*output, &error);
                if (error.error == QJsonParseError::NoError && doc.isObject()) {
                    const auto o = doc.object();
                    if (o.value(QStringLiteral("ok")).toBool())
                        result = o;
                    else
                        result = std::unexpected(o.value(QStringLiteral("error"))
                                                     .toString(tx(u"Shell 操作失败")));
                }
            }
            process->deleteLater();
            if (!stopping_)
                request.callback(std::move(result));
            pump();
        };
        connect(process, &QProcess::started, this, [process, request] {
            if (request.permitted && !request.permitted()) {
                process->kill();
                return;
            }
            process->write(QJsonDocument(request.payload).toJson(QJsonDocument::Compact));
            process->closeWriteChannel();
        });
        connect(process, &QProcess::readyReadStandardOutput, this, [process, output, timedOut] {
            output->append(process->readAllStandardOutput());
            if (output->size() > 8 * 1024 * 1024) {
                *timedOut = true;
                process->kill();
            }
        });
        connect(process, &QProcess::readyReadStandardError, this,
                [process] { process->readAllStandardError(); });
        connect(process, &QProcess::errorOccurred, this,
                [finish](QProcess::ProcessError error) mutable {
                    if (error == QProcess::FailedToStart)
                        finish(-1);
                });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [process, output, finish](int code, QProcess::ExitStatus) mutable {
                    output->append(process->readAllStandardOutput());
                    finish(code);
                });
        connect(timer, &QTimer::timeout, this, [process, timedOut] {
            *timedOut = true;
            process->kill();
        });
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments({QStringLiteral("--shell-helper")});
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->start();
        timer->start(request.timeout);
    }
}
void ShellJobs::icon(const QString& id, const QString& source, const QString& root, IconCallback callback) {
    const auto key = root + u'\n' + id + u'\n' + source;
    if (iconCache_.contains(key)) {
        QTimer::singleShot(
            0, this,
            [callback = std::move(callback), image = iconCache_.value(key)] { callback(image); });
        return;
    }
    iconWaiters_[key].append(std::move(callback));
    if (iconWaiters_[key].size() > 1)
        return;
    enqueue({{{QStringLiteral("operation"), QStringLiteral("icon")},
              {QStringLiteral("source"), source},
              {QStringLiteral("root"), root}},
             3000,
             [this, key](auto result) {
                 QImage image;
                 if (result)
                     image = QImage::fromData(QByteArray::fromBase64(
                         result->value(QStringLiteral("png")).toString().toLatin1()));
                 if (iconCache_.size() >= 256)
                     iconCache_.clear();
                 if (!image.isNull()) iconCache_.insert(key, image);
                 const auto waiters = iconWaiters_.take(key);
                 for (const auto& callback : waiters)
                     callback(image);
             }});
}
void ShellJobs::reveal(const QString& target, const QString& root,
                       std::function<void(std::expected<void, QString>)> callback) {
    enqueue({{{QStringLiteral("operation"), QStringLiteral("reveal")},
              {QStringLiteral("target"), target}, {QStringLiteral("root"), root}},
             10000, [callback = std::move(callback)](auto result) {
                 if (!result) callback(std::unexpected(result.error()));
                 else callback({});
             }});
}
void ShellJobs::launch(const LaunchItem& item, const QString& root,
                       std::function<void(std::expected<void, QString>)> callback,
                       std::function<bool()> permitted) {
    enqueue({{{QStringLiteral("operation"), QStringLiteral("launch")},
              {QStringLiteral("item"), itemObject(item)},
              {QStringLiteral("root"), root}},
             30000,
             [callback = std::move(callback)](auto result) {
                 if (!result)
                     callback(std::unexpected(result.error()));
                 else
                     callback({});
             }, std::move(permitted)});
}
void ShellJobs::importPaths(
    const QStringList& paths, const QString& root,
    std::function<void(std::expected<QList<LaunchItem>, QString>)> callback) {
    enqueue({{{QStringLiteral("operation"), QStringLiteral("import")},
              {QStringLiteral("paths"), QJsonArray::fromStringList(paths)},
              {QStringLiteral("root"), root}},
             10000,
             [callback = std::move(callback)](auto result) {
                 if (!result) {
                     callback(std::unexpected(result.error()));
                     return;
                 }
                 QList<LaunchItem> items;
                 for (const auto& value : result->value(QStringLiteral("items")).toArray())
                     items.append(itemValue(value.toObject()));
                 callback(std::move(items));
             }});
}
} // namespace mt
