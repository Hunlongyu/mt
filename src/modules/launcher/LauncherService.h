#pragma once
#include "LauncherCatalog.h"
#include "infrastructure/settings/Settings.h"
#include <QObject>
#include <QThread>
#include <functional>
#include <memory>

namespace mt {
// Mutations are queued on one worker. The GUI sees only durably saved snapshots.
class LauncherService final : public QObject {
    Q_OBJECT
  public:
    using Result = std::expected<void, QString>;
    using Completion = std::function<void(Result)>;
    using Save = std::function<Result(const LauncherCatalog&, const QString&)>;
    explicit LauncherService(QString path, QObject* parent = nullptr, Save save = {});
    ~LauncherService() override;
    Result load();   // startup only, before accepting commands
    void shutdown(); // drains accepted writes before joining the worker
    bool busy() const {
        return pending_ != 0;
    }
    const LauncherCatalog& catalog() const {
        return catalog_;
    }
    // bool means queued, not saved; completion is delivered on the GUI thread.
    bool put(const QString& category, int slot, LaunchItem item, Completion done = {});
    bool erase(const QString& category, int slot, Completion done = {});
    bool move(const QString& itemId, const QString& category, int slot, Completion done = {});
    bool insertBlank(const QString& category, int slot, Completion done = {});
    bool renameCategory(const QString& category, const QString& name, Completion done = {});
    bool addCategory(const QString& name, Completion done = {});
    bool removeCategory(const QString& category, Completion done = {});
    bool moveToCategory(const QString& itemId, const QString& category, Completion done = {});
    bool setColumns(int columns, Completion done = {});
    bool recordLaunch(const QString& itemId, Completion done = {});
    using MergeCompletion = std::function<void(std::expected<QPair<int, int>, QString>)>;
    void merge(const QList<ImportedCategory>& imported, MergeCompletion done);
    void importHLaunch(const QString& folder,
                       std::function<void(std::expected<QList<int>, QString>)> done);
    void addItems(const QString& category, int slot, const QList<LaunchItem>& items,
                  std::function<void(std::expected<int, QString>)> done);
    bool reorderCategory(const QString& category, int offset, Completion done = {});
    // A completed tab drag is one UUID permutation, never a chain of stale indices.
    bool reorderCategories(QStringList order, Completion done = {});
    bool selectCategory(const QString& category, Completion done = {});
    // Grid conversion and settings are one recoverable persistence transaction.
    void configure(SettingsStore store, Settings settings, Completion done);
  Q_SIGNALS:
    void changed();
    void failed(const QString& message);

  private:
    using Action = std::function<Result(LauncherCatalog&)>;
    using Persist = std::function<Result(const LauncherCatalog&, const LauncherCatalog&)>;
    bool change(Action action, Completion done, Persist persist = {});
    struct State;
    QString path_;
    LauncherCatalog catalog_;
    QThread thread_;
    QObject* worker_ = nullptr;
    std::shared_ptr<State> state_;
    int pending_ = 0;
    bool loaded_ = false;
    bool stopping_ = false;
};
} // namespace mt
