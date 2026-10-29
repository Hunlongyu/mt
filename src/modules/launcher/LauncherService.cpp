#include "LauncherService.h"
#include "infrastructure/i18n/I18n.h"
#include "infrastructure/storage/HLaunchImport.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace mt {
struct LauncherService::State {
    LauncherCatalog catalog;
    Save save;
};
LauncherService::LauncherService(QString path, QObject* parent, Save save)
    : QObject(parent), path_(std::move(path)), worker_(new QObject),
      state_(std::make_shared<State>()) {
    state_->save =
        save ? std::move(save) : Save([](const auto& c, const auto& p) { return c.save(p); });
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.start();
}
LauncherService::~LauncherService() {
    shutdown();
}
void LauncherService::shutdown() {
    if (stopping_)
        return;
    stopping_ = true;
    // Every accepted operation is already queued on this worker. A barrier drains
    // them without depending on GUI callbacks or running a nested event loop.
    QMetaObject::invokeMethod(worker_, [] {}, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}
LauncherService::Result LauncherService::load() {
    if (loaded_ || stopping_)
        return std::unexpected(tx(u"启动项未成功加载，禁止覆盖原文件"));
    auto c = LauncherCatalog::load(path_);
    if (!c)
        return std::unexpected(c.error());
    catalog_ = std::move(*c);
    QMetaObject::invokeMethod(worker_,
                              [state = state_, catalog = catalog_] { state->catalog = catalog; });
    loaded_ = true;
    Q_EMIT changed();
    return {};
}
bool LauncherService::change(Action action, Completion done, Persist persist) {
    if (pending_ >= 1024) {
        const auto message = tx(u"启动项保存队列已满，请稍后重试");
        Q_EMIT failed(message);
        if (done)
            done(std::unexpected(message));
        return false;
    }
    if (!loaded_ || stopping_) {
        const auto message = tx(u"启动项未成功加载，禁止覆盖原文件");
        Q_EMIT failed(message);
        if (done)
            done(std::unexpected(message));
        return false;
    }
    ++pending_;
    QMetaObject::invokeMethod(worker_, [this, state = state_, path = path_,
                                        action = std::move(action), done = std::move(done),
                                        persist = std::move(persist)]() mutable {
        auto next = state->catalog;
        Result result;
        // A failed transaction recovery forbids overwriting either file until
        // startup recovery succeeds.
        if (QFileInfo::exists(QDir(QFileInfo(path).absolutePath())
                                  .filePath(QStringLiteral("launcher-settings-rollback.json"))))
            result = std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
        else
            result = action(next);
        if (result)
            result = persist ? persist(state->catalog, next) : state->save(next, path);
        if (result)
            state->catalog = next;
        QMetaObject::invokeMethod(
            this, [this, next = std::move(next), result, done = std::move(done)]() mutable {
                --pending_;
                if (stopping_)
                    return;
                if (result) {
                    catalog_ = std::move(next);
                    Q_EMIT changed();
                } else {
                    Q_EMIT failed(result.error());
                }
                if (done)
                    done(result);
            });
    });
    return true;
}
bool LauncherService::put(const QString& category, int slot, LaunchItem item, Completion done) {
    return change([category, slot, item = std::move(item)](
                      auto& c) { return c.put(c.categoryIndex(category), slot, item); },
                  std::move(done));
}
bool LauncherService::erase(const QString& category, int slot, Completion done) {
    return change(
        [category, slot](auto& c) -> Result {
            if (!c.erase(c.categoryIndex(category), slot))
                return std::unexpected(tx(u"启动项不存在"));
            return {};
        },
        std::move(done));
}
bool LauncherService::move(const QString& item, const QString& category, int slot,
                           Completion done) {
    return change(
        [item, category, slot](auto& c) -> Result {
            for (int from = 0; from < c.categories.size(); ++from) {
                const int index = static_cast<int>(c.categories[from].slots.indexOf(item));
                if (index >= 0)
                    return c.move(from, index, c.categoryIndex(category), slot);
            }
            return std::unexpected(tx(u"拖动的启动项已不存在"));
        },
        std::move(done));
}
bool LauncherService::insertBlank(const QString& category, int slot, Completion done) {
    return change(
        [category, slot](auto& c) { return c.insertBlank(c.categoryIndex(category), slot); },
        std::move(done));
}
bool LauncherService::renameCategory(const QString& category, const QString& name,
                                     Completion done) {
    return change(
        [category, name](auto& c) -> Result {
            const int index = c.categoryIndex(category);
            if (index < 0 || name.trimmed().isEmpty())
                return std::unexpected(tx(u"分类名称不能为空"));
            c.categories[index].name = name.trimmed();
            return {};
        },
        std::move(done));
}
bool LauncherService::addCategory(const QString& name, Completion done) {
    return change(
        [name](auto& c) -> Result {
            if (name.trimmed().isEmpty())
                return std::unexpected(tx(u"分类名称不能为空"));
            LaunchCategory category{LauncherCatalog::newId(), name.trimmed(), {}};
            for (int i = 0; i < LauncherCatalog::slotBlock; ++i)
                category.slots.append(QString{});
            c.categories.append(category);
            c.selectedCategory = category.id;
            return {};
        },
        std::move(done));
}
bool LauncherService::removeCategory(const QString& category, Completion done) {
    return change([category](auto& c) { return c.removeCategory(c.categoryIndex(category)); },
                  std::move(done));
}
bool LauncherService::setColumns(int columns, Completion done) {
    return change([columns](auto& c) { return c.setColumns(columns); }, std::move(done));
}
bool LauncherService::moveToCategory(const QString& item, const QString& category,
                                     Completion done) {
    return change(
        [item, category](auto& c) { return c.moveToCategory(item, c.categoryIndex(category)); },
        std::move(done));
}
bool LauncherService::recordLaunch(const QString& item, Completion done) {
    return change(
        [item](auto& c) -> Result {
            c.recordLaunch(item, QDateTime::currentDateTimeUtc());
            return {};
        },
        std::move(done));
}
void LauncherService::merge(const QList<ImportedCategory>& imported, MergeCompletion done) {
    auto counts = std::make_shared<QPair<int, int>>(0, 0);
    change(
        [imported, counts](auto& c) -> Result {
            counts->first = c.merge(imported, &counts->second);
            return {};
        },
        [counts, done = std::move(done)](Result result) {
            if (done)
                done(result ? std::expected<QPair<int, int>, QString>(*counts)
                            : std::unexpected(result.error()));
        });
}
void LauncherService::importHLaunch(const QString& folder,
                                    std::function<void(std::expected<QList<int>, QString>)> done) {
    auto counts = std::make_shared<QList<int>>(QList<int>{0, 0, 0});
    change(
        [folder, root = QFileInfo(path_).absolutePath(), counts](auto& c) -> Result {
            auto data = readHLaunchData(folder, root, c.columns);
            if (!data)
                return std::unexpected(data.error());
            (*counts)[0] = c.merge(data->categories, &(*counts)[1]);
            (*counts)[2] = data->skipped;
            return {};
        },
        [counts, done = std::move(done)](Result result) {
            if (done)
                done(result ? std::expected<QList<int>, QString>(*counts)
                            : std::unexpected(result.error()));
        });
}
void LauncherService::addItems(const QString& category, int slot, const QList<LaunchItem>& items,
                               std::function<void(std::expected<int, QString>)> done) {
    auto skipped = std::make_shared<int>(0);
    change(
        [category, slot, items, skipped](auto& c) mutable -> Result {
            const int index = c.categoryIndex(category);
            if (index < 0)
                return std::unexpected(tx(u"目标分类已删除，未添加项目"));
            for (const auto& item : items) {
                if (!c.duplicateOf(item).isEmpty()) {
                    ++*skipped;
                    continue;
                }
                while (!c.categories[index].slots.value(slot).isEmpty())
                    ++slot;
                if (auto result = c.put(index, slot, item); !result)
                    return result;
                ++slot;
            }
            return {};
        },
        [skipped, done = std::move(done)](Result result) {
            if (done)
                done(result ? std::expected<int, QString>(*skipped)
                            : std::unexpected(result.error()));
        });
}
bool LauncherService::reorderCategory(const QString& category, int offset, Completion done) {
    return change(
        [category, offset](auto& c) -> Result {
            const int from = c.categoryIndex(category);
            const int to = from + offset;
            if (from < 0 || to < 0 || from >= c.categories.size() || to >= c.categories.size())
                return std::unexpected(tx(u"分类位置无效"));
            c.categories.move(from, to);
            return {};
        },
        std::move(done));
}
bool LauncherService::reorderCategories(QStringList order, Completion done) {
    return change(
        [order = std::move(order)](auto& c) -> Result {
            if (order.size() != c.categories.size())
                return std::unexpected(tx(u"分类位置无效"));
            QList<LaunchCategory> categories;
            QStringList seen;
            for (const auto& id : order) {
                const int index = c.categoryIndex(id);
                if (index < 0 || seen.contains(id))
                    return std::unexpected(tx(u"分类位置无效"));
                seen.append(id);
                categories.append(c.categories[index]);
            }
            c.categories = std::move(categories);
            return {};
        },
        std::move(done));
}
bool LauncherService::selectCategory(const QString& category, Completion done) {
    return change(
        [category](auto& c) -> Result {
            if (c.categoryIndex(category) < 0)
                return std::unexpected(tx(u"分类不存在"));
            c.selectedCategory = category;
            return {};
        },
        std::move(done));
}
void LauncherService::configure(SettingsStore store, Settings settings, Completion done) {
    change([columns = settings.launcherColumns](auto& c) { return c.setColumns(columns); },
           std::move(done),
           [store, settings, path = path_](const auto& before, const auto& after) {
               return store.saveLauncherConfiguration(settings, path, before, after);
           });
}
} // namespace mt
