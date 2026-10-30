#pragma once
#include <QByteArray>
#include <QString>
#include <expected>
namespace mt {
// The helper is a copy of the running, already trusted executable, never the download.
std::expected<QString, QString> prepareUpdateInstaller(const QString& candidate,
    const QByteArray& sha256, qint64 bytes, const QString& dataRoot);
int runUpdateInstaller(const QString& planPath, bool preview = false);
void acknowledgeUpdateStartup(const QString& stage);
// Best-effort removal of a finished temporary updater, retaining rollback EXEs
// and failed stages. Never scans or removes application data.
void cleanupCompletedUpdateHelpers(const QString& executable);
std::expected<void, QString> verifyUpdateExecutable(const QString& path,
    const QByteArray& sha256, qint64 bytes);
} // namespace mt
