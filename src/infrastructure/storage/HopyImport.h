#pragma once
#include "modules/clipboard/ClipboardRecord.h"
#include <expected>

namespace mt {
struct HopyHistory {
    int imported = 0;
    int skipped = 0; // rows whose sidecar file is missing or whose payload exceeds the limits
};
// Reads a Hopy data folder (clipboard.sqlite plus images/ and rich_text/).
// The database is copied to a private temporary folder first and opened
// read-only there, so the original files are never locked or modified.
// Run on a worker thread. Each payload is passed to consume before the next
// image is decoded; a consumer error aborts the traversal.
std::expected<HopyHistory, QString> readHopyHistory(const QString& folder,
                                                   const ClipConsumer& consume);
// The folder a default Hopy installation uses for the current user.
QString defaultHopyFolder();
} // namespace mt
