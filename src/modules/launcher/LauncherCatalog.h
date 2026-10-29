#pragma once
#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QStringList>
#include <QVector>
#include <expected>

namespace mt {
struct LaunchItem {
    QString id;
    QString name;
    QString target;
    QString arguments;
    QString workingDirectory;
    QString iconPath;
    bool elevated = false;
    // Usage statistics; counted only after the Shell accepted a launch.
    qint64 launchCount = 0;
    QDateTime lastLaunched; // UTC, invalid when never launched
};
struct LaunchCategory {
    QString id;
    QString name;
    QStringList slots;
};
// Items from another launcher with their preferred slot in a named category.
struct ImportedCategory {
    QString name;
    QList<QPair<int, LaunchItem>> items;
};
struct LaunchMatch {
    QString itemId;
    QString categoryId;
    int slot = 0;
    int score = 0;
};
class LauncherCatalog {
  public:
    // The grid size is a user setting (DEC-032): 3–20 columns and rows,
    // 4 × 7 by default. Slots are row-major for the column count stored here.
    static constexpr int defaultColumns = 4;
    static constexpr int defaultRows = 7;
    static constexpr int minGrid = 3;
    static constexpr int maxGrid = 20;
    static constexpr int maxSlots = 10000;
    // Slot lists grow in blocks of this size; padding has no layout meaning.
    static constexpr int slotBlock = defaultColumns * defaultRows;
    int columns = defaultColumns;
    QList<LaunchCategory> categories;
    QHash<QString, LaunchItem> items;
    QString selectedCategory;
    static LauncherCatalog empty();
    static std::expected<LauncherCatalog, QString> load(const QString& path);
    std::expected<void, QString> save(const QString& path) const;
    QByteArray encode() const;
    std::expected<void, QString> validate() const;
    // Re-lays every category out for a new column count. Items keep their row
    // and column; items in removed columns continue, in order, from the start
    // of the next row without filling earlier gaps. Rows are only a page size.
    std::expected<void, QString> setColumns(int columns);
    std::expected<void, QString> put(int category, int slot, LaunchItem item);
    std::expected<void, QString> move(int fromCategory, int fromSlot, int toCategory, int toSlot);
    bool erase(int category, int slot);
    std::expected<void, QString> insertBlank(int category, int slot);
    // Places the item after the last occupied slot of the category.
    std::expected<int, QString> append(int category, LaunchItem item);
    // Moves an existing item to the end of another category.
    std::expected<void, QString> moveToCategory(const QString& itemId, int category);
    // Deletes a category; its items move to the end of a neighbouring one.
    std::expected<void, QString> removeCategory(int category);
    bool recordLaunch(const QString& itemId, const QDateTime& when);
    // Adds imported items to the category of the same name (created if missing),
    // each at its preferred slot or the next free one; exact duplicates are
    // skipped. Returns the number added.
    int merge(const QList<ImportedCategory>& imported, int* duplicates = nullptr);
    // Id of an item with the same target, arguments, folder and elevation
    // (display name and id do not matter).
    QString duplicateOf(const LaunchItem& item) const;
    QList<LaunchMatch> search(const QString& query) const;
    int categoryIndex(const QString& id) const;
    static QString newId();
    // Windows command-line quoting for one argument (CommandLineToArgvW rules).
    static QString quoteArgument(const QString& argument);
    static QString commandLine(const LaunchItem& item);
    // Programs and shortcuts accept dropped files as one-off arguments.
    static bool acceptsFileArguments(const LaunchItem& item);
    static bool isUrl(const QString& target);
};
} // namespace mt
Q_DECLARE_METATYPE(mt::LaunchItem)
