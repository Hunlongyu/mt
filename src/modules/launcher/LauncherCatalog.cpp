#include "LauncherCatalog.h"
#include "infrastructure/i18n/I18n.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace mt {
namespace {
void grow(QStringList& slots, int index) {
    if (slots.size() <= index) {
        const int size = (std::min)(LauncherCatalog::maxSlots,
            (index / LauncherCatalog::slotBlock + 1) * LauncherCatalog::slotBlock);
        slots.resize(size);
    }
}
} // namespace
QString LauncherCatalog::newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
LauncherCatalog LauncherCatalog::empty() {
    LauncherCatalog c;
    for (const auto& name : {tx(u"常用"), tx(u"办公"), tx(u"开发"),
                             tx(u"收藏")}) {
        LaunchCategory category{newId(), name, {}};
        grow(category.slots, 0);
        c.categories.append(category);
    }
    c.selectedCategory = c.categories[2].id;
    return c;
}
int LauncherCatalog::categoryIndex(const QString& id) const {
    for (int i = 0; i < categories.size(); ++i)
        if (categories[i].id == id)
            return i;
    return -1;
}
std::expected<void, QString> LauncherCatalog::validate() const {
    if (columns < minGrid || columns > maxGrid)
        return std::unexpected(tx(u"启动面板列数无效"));
    if (categories.isEmpty() || categories.size() > 100)
        return std::unexpected(tx(u"分类数量无效"));
    QSet<QString> categoryIds, used;
    for (const auto& c : categories) {
        if (c.id.isEmpty() || c.name.trimmed().isEmpty() || categoryIds.contains(c.id) ||
            c.slots.size() > maxSlots)
            return std::unexpected(tx(u"分类或槽位数据无效"));
        categoryIds.insert(c.id);
        for (const auto& id : c.slots) {
            if (id.isEmpty())
                continue;
            if (!items.contains(id) || used.contains(id))
                return std::unexpected(tx(u"启动项引用缺失或重复"));
            used.insert(id);
        }
    }
    if (used.size() != items.size())
        return std::unexpected(tx(u"存在未关联的启动项"));
    for (auto i = items.cbegin(); i != items.cend(); ++i)
        if (i.key() != i->id || i->name.trimmed().isEmpty() || i->target.trimmed().isEmpty())
            return std::unexpected(tx(u"启动项名称或目标无效"));
    if (!categoryIds.contains(selectedCategory))
        return std::unexpected(tx(u"选中分类无效"));
    return {};
}
std::expected<LauncherCatalog, QString> LauncherCatalog::load(const QString& path) {
    QFile file(path);
    if (!file.exists())
        return empty();
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(file.errorString());
    if (file.size() > 16 * 1024 * 1024)
        return std::unexpected(tx(u"启动项文件异常大，已保留"));
    QJsonParseError e;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(tx(u"启动项文件解析失败，已保留"));
    const auto root = doc.object();
    if (root.value(QStringLiteral("schema")).toInt() != 1)
        return std::unexpected(tx(u"不支持此启动项版本，已保留"));
    LauncherCatalog c;
    c.selectedCategory = root.value(QStringLiteral("selectedCategory")).toString();
    // Files written before the grid size was configurable are 4 columns wide.
    c.columns = root.value(QStringLiteral("columns")).toInt(defaultColumns);
    for (const auto& value : root.value(QStringLiteral("categories")).toArray()) {
        const auto o = value.toObject();
        LaunchCategory category;
        category.id = o.value(QStringLiteral("id")).toString();
        category.name = o.value(QStringLiteral("name")).toString();
        for (const auto& slot : o.value(QStringLiteral("slots")).toArray())
            category.slots.append(slot.toString());
        c.categories.append(category);
    }
    for (const auto& value : root.value(QStringLiteral("items")).toArray()) {
        const auto o = value.toObject();
        LaunchItem item;
        item.id = o.value(QStringLiteral("id")).toString();
        item.name = o.value(QStringLiteral("name")).toString();
        item.target = o.value(QStringLiteral("target")).toString();
        item.arguments = o.value(QStringLiteral("arguments")).toString();
        item.workingDirectory = o.value(QStringLiteral("workingDirectory")).toString();
        item.iconPath = o.value(QStringLiteral("iconPath")).toString();
        item.elevated = o.value(QStringLiteral("elevated")).toBool();
        // Optional since the first release; older files simply have no statistics.
        item.launchCount = std::max<qint64>(
            0, static_cast<qint64>(o.value(QStringLiteral("launchCount")).toDouble()));
        item.lastLaunched = QDateTime::fromString(
            o.value(QStringLiteral("lastLaunchedAt")).toString(), Qt::ISODate);
        if (c.items.contains(item.id))
            return std::unexpected(tx(u"启动项 ID 重复"));
        c.items.insert(item.id, item);
    }
    if (auto check = c.validate(); !check)
        return std::unexpected(check.error());
    return c;
}
std::expected<void, QString> LauncherCatalog::save(const QString& path) const {
    if (auto check = validate(); !check)
        return check;
    const auto data = encode();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return std::unexpected(tx(u"无法创建启动项目录"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        return std::unexpected(f.errorString());
    return {};
}
QByteArray LauncherCatalog::encode() const {
    QJsonArray cats, entries;
    for (const auto& c : categories)
        cats.append(QJsonObject{{QStringLiteral("id"), c.id},
                                {QStringLiteral("name"), c.name},
                                {QStringLiteral("slots"), QJsonArray::fromStringList(c.slots)}});
    auto ids = items.keys();
    ids.sort();
    for (const auto& id : ids) {
        const auto& i = items[id];
        QJsonObject entry{{QStringLiteral("id"), i.id},
                          {QStringLiteral("name"), i.name},
                          {QStringLiteral("target"), i.target},
                          {QStringLiteral("arguments"), i.arguments},
                          {QStringLiteral("workingDirectory"), i.workingDirectory},
                          {QStringLiteral("iconPath"), i.iconPath},
                          {QStringLiteral("elevated"), i.elevated}};
        if (i.launchCount > 0)
            entry.insert(QStringLiteral("launchCount"), static_cast<double>(i.launchCount));
        if (i.lastLaunched.isValid())
            entry.insert(QStringLiteral("lastLaunchedAt"),
                         i.lastLaunched.toUTC().toString(Qt::ISODate));
        entries.append(entry);
    }
    return
        QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
                                  {QStringLiteral("selectedCategory"), selectedCategory},
                                  {QStringLiteral("columns"), columns},
                                  {QStringLiteral("categories"), cats},
                                  {QStringLiteral("items"), entries}})
            .toJson();
}
std::expected<void, QString> LauncherCatalog::put(int category, int slot, LaunchItem item) {
    if (category < 0 || category >= categories.size() || slot < 0 || slot >= maxSlots ||
        item.name.trimmed().isEmpty() || item.target.trimmed().isEmpty())
        return std::unexpected(tx(u"启动项参数无效"));
    auto& slots = categories[category].slots;
    grow(slots, slot);
    if (item.id.isEmpty())
        item.id = newId();
    if (items.contains(item.id) && slots[slot] != item.id)
        return std::unexpected(tx(u"此启动项已存在于其他槽位"));
    if (!slots[slot].isEmpty() && slots[slot] != item.id)
        return std::unexpected(tx(u"目标槽位已占用"));
    slots[slot] = item.id;
    items.insert(item.id, std::move(item));
    return {};
}
std::expected<void, QString> LauncherCatalog::move(int fromCategory, int fromSlot, int toCategory,
                                                   int toSlot) {
    if (fromCategory < 0 || fromCategory >= categories.size() || toCategory < 0 ||
        toCategory >= categories.size() || fromSlot < 0 ||
        fromSlot >= categories[fromCategory].slots.size() || toSlot < 0 || toSlot >= maxSlots)
        return std::unexpected(tx(u"移动位置无效"));
    auto& source = categories[fromCategory].slots;
    if (source[fromSlot].isEmpty())
        return {};
    auto& destination = categories[toCategory].slots;
    grow(destination, toSlot);
    if (fromCategory == toCategory && fromSlot == toSlot)
        return {};
    const auto id = source[fromSlot];
    if (destination[toSlot].isEmpty()) {
        source[fromSlot].clear();
        destination[toSlot] = id;
        return {};
    }
    if (fromCategory == toCategory) {
        const int step = fromSlot < toSlot ? 1 : -1;
        for (int i = fromSlot; i != toSlot; i += step)
            source[i] = source[i + step];
        source[toSlot] = id;
    } else {
        int vacancy = toSlot;
        while (vacancy < destination.size() && !destination[vacancy].isEmpty())
            ++vacancy;
        if (vacancy >= maxSlots)
            return std::unexpected(tx(u"分类已达到容量上限"));
        grow(destination, vacancy);
        for (int i = vacancy; i > toSlot; --i)
            destination[i] = destination[i - 1];
        destination[toSlot] = id;
        source[fromSlot].clear();
    }
    return {};
}
bool LauncherCatalog::erase(int category, int slot) {
    if (category < 0 || category >= categories.size() || slot < 0 ||
        slot >= categories[category].slots.size())
        return false;
    auto& id = categories[category].slots[slot];
    if (id.isEmpty())
        return false;
    items.remove(id);
    id.clear();
    return true;
}
std::expected<void, QString> LauncherCatalog::insertBlank(int category, int slot) {
    if (category < 0 || category >= categories.size() || slot < 0 || slot >= maxSlots)
        return std::unexpected(tx(u"启动项参数无效"));
    auto& slots = categories[category].slots;
    int vacancy = slot;
    while (vacancy < maxSlots && !slots.value(vacancy).isEmpty())
        ++vacancy;
    if (vacancy >= maxSlots)
        return std::unexpected(tx(u"分类已达到容量上限"));
    grow(slots, vacancy);
    for (int i = vacancy; i > slot; --i)
        slots[i] = slots[i - 1];
    slots[slot].clear();
    return {};
}
std::expected<void, QString> LauncherCatalog::setColumns(int next) {
    if (next < minGrid || next > maxGrid)
        return std::unexpected(tx(u"启动面板列数无效"));
    if (next == columns)
        return {};
    for (auto& category : categories) {
        QStringList slots;
        QList<QPair<int, QString>> overflow;
        const auto place = [&slots](int slot, const QString& id) {
            grow(slots, slot);
            slots[slot] = id;
        };
        // Items that keep their column go first; their targets never collide.
        for (int slot = 0; slot < category.slots.size(); ++slot) {
            const auto& id = category.slots[slot];
            if (id.isEmpty())
                continue;
            const int row = slot / columns;
            const int column = slot % columns;
            if (column >= next) {
                overflow.append({(row + 1) * next, id});
                continue;
            }
            const int target = row * next + column;
            if (target >= maxSlots)
                return std::unexpected(tx(u"分类已达到容量上限"));
            place(target, id);
        }
        for (auto [slot, id] : overflow) {
            while (slot < maxSlots && !slots.value(slot).isEmpty())
                ++slot;
            if (slot >= maxSlots)
                return std::unexpected(tx(u"分类已达到容量上限"));
            place(slot, id);
        }
        grow(slots, 0);
        category.slots = slots;
    }
    columns = next;
    return {};
}
std::expected<int, QString> LauncherCatalog::append(int category, LaunchItem item) {
    if (category < 0 || category >= categories.size())
        return std::unexpected(tx(u"启动项参数无效"));
    const auto& slots = categories[category].slots;
    int slot = static_cast<int>(slots.size());
    while (slot > 0 && slots[slot - 1].isEmpty())
        --slot;
    if (auto result = put(category, slot, std::move(item)); !result)
        return std::unexpected(result.error());
    return slot;
}
std::expected<void, QString> LauncherCatalog::moveToCategory(const QString& itemId, int category) {
    if (category < 0 || category >= categories.size() || !items.contains(itemId))
        return std::unexpected(tx(u"移动位置无效"));
    if (categories[category].slots.contains(itemId))
        return {};
    for (auto& c : categories)
        if (const auto index = c.slots.indexOf(itemId); index >= 0) {
            c.slots[index].clear();
            break;
        }
    auto item = items.take(itemId);
    if (auto result = append(category, std::move(item)); !result)
        return std::unexpected(result.error());
    return {};
}
std::expected<void, QString> LauncherCatalog::removeCategory(int category) {
    if (category < 0 || category >= categories.size() || categories.size() == 1)
        return std::unexpected(tx(u"至少保留一个分类"));
    // Items are never dropped with their category: they join the neighbour.
    const int destination = category > 0 ? category - 1 : 1;
    const auto removed = categories[category];
    for (const auto& id : removed.slots) {
        if (id.isEmpty())
            continue;
        if (auto result = append(destination, items.take(id)); !result)
            return std::unexpected(result.error());
    }
    categories.removeAt(category);
    if (selectedCategory == removed.id)
        selectedCategory = categories[category > 0 ? category - 1 : 0].id;
    return {};
}
bool LauncherCatalog::recordLaunch(const QString& itemId, const QDateTime& when) {
    const auto it = items.find(itemId);
    if (it == items.end())
        return false;
    ++it->launchCount;
    it->lastLaunched = when.toUTC();
    return true;
}
int LauncherCatalog::merge(const QList<ImportedCategory>& imported, int* duplicates) {
    int added = 0;
    for (const auto& source : imported) {
        int index = -1;
        for (int i = 0; i < categories.size(); ++i)
            if (categories[i].name.trimmed().compare(source.name.trimmed(), Qt::CaseInsensitive) ==
                0)
                index = i;
        if (index < 0) {
            if (source.items.isEmpty() || categories.size() >= 100)
                continue;
            LaunchCategory category{newId(), source.name.trimmed(), {}};
            grow(category.slots, 0);
            categories.append(category);
            index = static_cast<int>(categories.size()) - 1;
        }
        for (auto [slot, item] : source.items) {
            if (!duplicateOf(item).isEmpty()) {
                if (duplicates)
                    ++*duplicates;
                continue;
            }
            if (item.id.isEmpty() || items.contains(item.id))
                item.id = newId();
            const auto& slots = categories[index].slots;
            while (slot < maxSlots && !slots.value(slot).isEmpty())
                ++slot;
            if (slot >= maxSlots || !put(index, slot, item))
                continue;
            ++added;
        }
    }
    return added;
}
QString LauncherCatalog::duplicateOf(const LaunchItem& item) const {
    const auto same = [](const QString& a, const QString& b) {
        return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
    };
    for (const auto& existing : items)
        if ((isUrl(existing.target) || isUrl(item.target)
                 ? QUrl(existing.target.trimmed()) == QUrl(item.target.trimmed())
                 : same(existing.target, item.target)) &&
            existing.arguments.trimmed() == item.arguments.trimmed() &&
            same(existing.workingDirectory, item.workingDirectory) &&
            existing.elevated == item.elevated)
            return existing.id;
    return {};
}
QString LauncherCatalog::quoteArgument(const QString& argument) {
    static const QRegularExpression special(QStringLiteral("[\\s\"]"));
    if (!argument.isEmpty() && !argument.contains(special))
        return argument;
    QString out(u'"');
    int backslashes = 0;
    for (const auto ch : argument) {
        if (ch == u'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes are literal unless they precede a quote.
        out += QString(ch == u'"' ? backslashes * 2 + 1 : backslashes, u'\\');
        backslashes = 0;
        out += ch;
    }
    out += QString(backslashes * 2, u'\\');
    out += u'"';
    return out;
}
QString LauncherCatalog::commandLine(const LaunchItem& item) {
    auto command = isUrl(item.target) ? item.target.trimmed() : quoteArgument(item.target.trimmed());
    if (!item.arguments.trimmed().isEmpty())
        command += u' ' + item.arguments.trimmed();
    return command;
}
bool LauncherCatalog::isUrl(const QString& target) {
    // "C:\..." parses with a one-letter scheme; real schemes are longer.
    return QUrl(target.trimmed()).scheme().size() > 1;
}
bool LauncherCatalog::acceptsFileArguments(const LaunchItem& item) {
    if (isUrl(item.target))
        return false;
    static const QStringList programs{QStringLiteral("exe"), QStringLiteral("com"),
                                      QStringLiteral("bat"), QStringLiteral("cmd"),
                                      QStringLiteral("lnk")};
    return programs.contains(QFileInfo(item.target.trimmed()).suffix().toLower());
}
QList<LaunchMatch> LauncherCatalog::search(const QString& query) const {
    struct Ranked {
        LaunchMatch match;
        int tier; // exact > prefix > substring > fuzzy > target path
    };
    QList<Ranked> ranked;
    const auto needle = query.trimmed().toCaseFolded();
    if (needle.isEmpty())
        return {};
    for (const auto& c : categories)
        for (int slot = 0; slot < c.slots.size(); ++slot) {
            if (c.slots[slot].isEmpty())
                continue;
            const auto& item = items[c.slots[slot]];
            const auto name = item.name.toCaseFolded();
            int tier = -1;
            int score = 0;
            if (name == needle)
                tier = 4;
            else if (name.startsWith(needle)) {
                tier = 3;
                score = -static_cast<int>(name.size());
            } else if (const auto pos = name.indexOf(needle); pos >= 0) {
                tier = 2;
                score = -static_cast<int>(pos);
            } else {
                qsizetype cursor = 0;
                int gaps = 0;
                bool matches = true;
                for (const auto ch : needle) {
                    const auto next = name.indexOf(ch, cursor);
                    if (next < 0) {
                        matches = false;
                        break;
                    }
                    gaps += static_cast<int>(next - cursor);
                    cursor = next + 1;
                }
                if (matches) {
                    tier = 1;
                    score = -gaps;
                } else if (item.target.contains(needle, Qt::CaseInsensitive))
                    tier = 0;
            }
            if (tier >= 0)
                ranked.append({{item.id, c.id, slot, tier * 100000 + std::max(score, -99999)},
                               tier});
        }
    // Within a match tier, frequently and recently used items come first; the
    // finer match score and then grid order break the remaining ties.
    std::stable_sort(ranked.begin(), ranked.end(), [this](const Ranked& a, const Ranked& b) {
        if (a.tier != b.tier)
            return a.tier > b.tier;
        const auto& x = items[a.match.itemId];
        const auto& y = items[b.match.itemId];
        if (x.launchCount != y.launchCount)
            return x.launchCount > y.launchCount;
        if (x.lastLaunched != y.lastLaunched)
            return x.lastLaunched > y.lastLaunched;
        return a.match.score > b.match.score;
    });
    QList<LaunchMatch> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked)
        out.append(r.match);
    return out;
}
} // namespace mt
