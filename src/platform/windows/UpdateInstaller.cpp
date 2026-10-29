#include "UpdateInstaller.h"
#include "infrastructure/updates/ReleaseInfo.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <windows.h>
#include <wil/resource.h>

namespace mt {
namespace {
bool writeFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QString processPath(HANDLE process) {
    wchar_t path[32768];
    DWORD length = static_cast<DWORD>(std::size(path));
    return QueryFullProcessImageNameW(process, 0, path, &length)
               ? QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).canonicalFilePath()
               : QString{};
}
QString creationTime(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return {};
    return QString::number((quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
}
bool validStage(const QString& stage, const QString& executable) {
    static const QRegularExpression name(QStringLiteral("^\\.mt-update-[a-f0-9-]{36}$"));
    const QFileInfo directory(stage), target(executable);
    return directory.isDir() && !directory.isSymLink() &&
           name.match(directory.fileName()).hasMatch() &&
           directory.absolutePath().compare(target.absolutePath(), Qt::CaseInsensitive) == 0;
}
}
std::expected<void, QString> verifyUpdateExecutable(const QString& path,
    const QByteArray& digest, qint64 bytes) {
    if (digest.size() != 32 || bytes < 64 || bytes > maxUpdateBytes)
        return std::unexpected(QStringLiteral("更新校验参数无效"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != bytes)
        return std::unexpected(QStringLiteral("更新文件无法读取或大小不符"));
    const auto header = file.read(64);
    if (header.size() != 64 || !header.startsWith("MZ"))
        return std::unexpected(QStringLiteral("更新文件不是 Windows 可执行程序"));
    const auto pe = qFromLittleEndian<quint32>(header.constData() + 60);
    if (pe > static_cast<quint64>(bytes - 6) || !file.seek(pe))
        return std::unexpected(QStringLiteral("更新文件的 PE 头无效"));
    const auto signature = file.read(6);
    if (signature.size() != 6 || signature.left(4) != QByteArray("PE\0\0", 4) ||
        qFromLittleEndian<quint16>(signature.constData() + 4) != 0x8664)
        return std::unexpected(QStringLiteral("更新文件不是 Windows x64 程序"));
    if (!file.seek(0)) return std::unexpected(QStringLiteral("更新文件读取失败"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result() != digest)
        return std::unexpected(QStringLiteral("更新文件 SHA-256 校验失败"));
    return {};
}
std::expected<QString, QString> prepareUpdateInstaller(const QString& candidate,
    const QByteArray& digest, qint64 bytes, const QString& root) {
    if (auto verified = verifyUpdateExecutable(candidate, digest, bytes); !verified)
        return std::unexpected(verified.error());
    const QFileInfo target(QCoreApplication::applicationFilePath());
    if (target.isSymLink() || target.canonicalFilePath().isEmpty())
        return std::unexpected(QStringLiteral("无法确定当前程序的位置"));
    const auto folder = target.dir().filePath(QStringLiteral(".mt-update-") +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkdir(folder))
        return std::unexpected(QStringLiteral("程序目录不可写，请将 MT 移到有写入权限的目录后重试"));
    const QDir stage(folder);
    // Remove only the newly created staging folder if preparation fails.
    auto cleanup = wil::scope_exit([&] { QDir(folder).removeRecursively(); });
    if (!QFile::copy(candidate, stage.filePath(QStringLiteral("candidate.exe"))) ||
        !QFile::copy(target.absoluteFilePath(), stage.filePath(QStringLiteral("updater.exe"))))
        return std::unexpected(QStringLiteral("无法暂存更新文件，当前程序未改变"));
    QJsonObject plan{{QStringLiteral("target"), target.canonicalFilePath()},
                     {QStringLiteral("parent"), static_cast<qint64>(GetCurrentProcessId())},
                     {QStringLiteral("created"), creationTime(GetCurrentProcess())},
                     {QStringLiteral("sha256"), QString::fromLatin1(digest.toHex())},
                     {QStringLiteral("bytes"), bytes},
                     {QStringLiteral("dataRoot"), QFileInfo(root).absoluteFilePath()}};
    const auto path = stage.filePath(QStringLiteral("plan.json"));
    if (!writeFile(path, QJsonDocument(plan).toJson(QJsonDocument::Compact)))
        return std::unexpected(QStringLiteral("无法保存更新计划"));
    QProcess helper;
    helper.setProgram(stage.filePath(QStringLiteral("updater.exe")));
    helper.setArguments({QStringLiteral("--apply-update"), path});
    helper.setWorkingDirectory(stage.absolutePath());
    if (!helper.startDetached())
        return std::unexpected(QStringLiteral("无法启动更新助手，当前程序继续运行"));
    cleanup.release();
    return folder;
}
int runUpdateInstaller(const QString& path, bool preview) {
    const QFileInfo planInfo(path);
    const QDir stage(planInfo.absolutePath());
    const auto report = [&](const QString& text) {
        writeFile(stage.filePath(QStringLiteral("error.txt")), text.toUtf8());
        return 1;
    };
    QFile file(path);
    if (planInfo.fileName() != QStringLiteral("plan.json") ||
        planInfo.absolutePath().compare(QCoreApplication::applicationDirPath(), Qt::CaseInsensitive) != 0 ||
        !file.open(QIODevice::ReadOnly) || file.size() > 16384)
        return 2;
    const auto json = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!json.isObject()) return 2;
    const auto plan = json.object();
    const QString target = plan.value(QStringLiteral("target")).toString();
    const auto dataRoot = plan.value(QStringLiteral("dataRoot")).toString();
    const QFileInfo targetInfo(target);
    if (!validStage(stage.absolutePath(), target) || targetInfo.isSymLink() ||
        targetInfo.canonicalFilePath().compare(target, Qt::CaseInsensitive) != 0 ||
        !QDir::isAbsolutePath(dataRoot))
        return report(QStringLiteral("更新目标或数据目录无效"));
    const auto parentId = plan.value(QStringLiteral("parent")).toInteger();
    if (parentId <= 0 || parentId > MAXDWORD) return report(QStringLiteral("更新父进程无效"));
    wil::unique_handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                          FALSE, static_cast<DWORD>(parentId)));
    if (!parent || processPath(parent.get()).compare(target, Qt::CaseInsensitive) != 0 ||
        creationTime(parent.get()) != plan.value(QStringLiteral("created")).toString())
        return report(QStringLiteral("原程序身份已变化，未执行更新"));
    const auto key = QCryptographicHash::hash(target.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto mutexName = (QStringLiteral("Local\\mt.Update.") + QString::fromLatin1(key)).toStdWString();
    wil::unique_handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!mutex) return report(QStringLiteral("无法建立更新锁"));
    const auto locked = WaitForSingleObject(mutex.get(), 0);
    if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED)
        return report(QStringLiteral("另一个更新正在进行"));
    const auto unlock = wil::scope_exit([&] { ReleaseMutex(mutex.get()); });
    const auto candidate = stage.filePath(QStringLiteral("candidate.exe"));
    const auto digestText = plan.value(QStringLiteral("sha256")).toString();
    static const QRegularExpression digestPattern(QStringLiteral("^[a-f0-9]{64}$"));
    if (!digestPattern.match(digestText).hasMatch()) return report(QStringLiteral("校验信息无效"));
    const auto digest = QByteArray::fromHex(digestText.toLatin1());
    const auto bytes = plan.value(QStringLiteral("bytes")).toInteger(-1);
    if (auto verified = verifyUpdateExecutable(candidate, digest, bytes); !verified)
        return report(verified.error());
    if (!writeFile(stage.filePath(QStringLiteral("ready")), QByteArrayLiteral("ready"))) return 2;
    // The host closes its own modules normally; never terminate an unresponsive host.
    QElapsedTimer waiting;
    waiting.start();
    while (WaitForSingleObject(parent.get(), 100) != WAIT_OBJECT_0) {
        if (QFileInfo::exists(stage.filePath(QStringLiteral("cancelled"))))
            return report(QStringLiteral("宿主已取消安装"));
        if (waiting.elapsed() >= 45000)
            return report(QStringLiteral("原程序未退出，已取消更新"));
    }
    if (QFileInfo::exists(stage.filePath(QStringLiteral("cancelled"))))
        return report(QStringLiteral("宿主已取消安装"));
    parent.reset();
    QStringList launchArguments{QStringLiteral("--data-dir"), dataRoot};
    // Used by the isolated installer check: the replacement must not activate real input hooks.
    if (preview)
        launchArguments.append({QStringLiteral("--preview"), QStringLiteral("--quit-after"), QStringLiteral("2500")});
    const auto restartOriginal = [&] {
        return QProcess::startDetached(target, launchArguments, targetInfo.absolutePath());
    };
    if (auto verified = verifyUpdateExecutable(candidate, digest, bytes); !verified) {
        restartOriginal();
        return report(verified.error());
    }
    const auto backup = stage.filePath(QStringLiteral("previous.exe"));
    if (!ReplaceFileW(target.toStdWString().c_str(), candidate.toStdWString().c_str(),
                       backup.toStdWString().c_str(), 0, nullptr, nullptr)) {
        const auto error = GetLastError();
        // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 can leave the original only at the backup path.
        if (!QFileInfo::exists(target) && QFileInfo::exists(backup))
            QFile::rename(backup, target);
        if (QFileInfo::exists(target)) restartOriginal();
        return report(QStringLiteral("替换程序失败（%1），已尝试恢复原程序；详情目录：%2").arg(error).arg(stage.path()));
    }
    const auto rollback = [&] {
        if (ReplaceFileW(target.toStdWString().c_str(), backup.toStdWString().c_str(),
                         nullptr, 0, nullptr, nullptr))
            restartOriginal();
    };
    if (auto verified = verifyUpdateExecutable(target, digest, bytes); !verified) {
        rollback();
        return report(verified.error());
    }
    QProcess child;
    child.setProgram(target);
    auto childArguments = launchArguments;
    childArguments.append({QStringLiteral("--update-complete"), stage.absolutePath()});
    child.setArguments(childArguments);
    child.setWorkingDirectory(targetInfo.absolutePath());
    if (!child.startDetached()) {
        rollback();
        return report(QStringLiteral("新版本无法启动，已尝试恢复原程序"));
    }
    writeFile(stage.filePath(QStringLiteral("installed")), QByteArrayLiteral("installed"));
    return 0;
}
void acknowledgeUpdateStartup(const QString& stage) {
    if (validStage(stage, QCoreApplication::applicationFilePath()))
        writeFile(QDir(stage).filePath(QStringLiteral("startup.ok")), QByteArrayLiteral("ok"));
}
} // namespace mt
