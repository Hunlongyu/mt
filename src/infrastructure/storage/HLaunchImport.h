#pragma once
#include "modules/launcher/LauncherCatalog.h"
#include <expected>

namespace mt {
struct HLaunchData {
    QList<ImportedCategory> categories;
    int skipped = 0; // invalid entries, unsupported paths or unreadable shortcut copies
};
// Reads HLaunch's items.json (schema 1) from its data folder or the folder that
// holds the EXE and its data subfolder. The source is only read: relative paths
// are made absolute against the HLaunch EXE folder, grid slots are remapped from
// HLaunch's column count to the given one, and shortcut copies that HLaunch kept in
// its own shortcuts folder are copied into dataRoot/shortcuts.
std::expected<HLaunchData, QString> readHLaunchData(
    const QString& folder, const QString& dataRoot,
    int columns = LauncherCatalog::defaultColumns);
// The data folder HLaunch falls back to when its EXE folder is not writable.
QString defaultHLaunchFolder();
} // namespace mt
