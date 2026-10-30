#include "StartupRegistration.h"
#include <QDir>
#include <QFileInfo>
#include <vector>
#include <wil/resource.h>
#include <windows.h>

namespace mt {
namespace {
QString quote(QString value) {
    // Paths cannot contain quotes; double trailing slashes before the closing quote.
    qsizetype trailing = 0;
    for (auto i = value.size(); i > 0 && value[i - 1] == u'\\'; --i)
        ++trailing;
    return u'"' + value + QString(trailing, u'\\') + u'"';
}
QString failure(LSTATUS code) {
    return QStringLiteral("无法更新 MT 开机启动（系统错误 %1）").arg(code);
}
} // namespace
StartupRegistration::StartupRegistration(QString executable, QString dataRoot, QString key,
                                         QString name)
    : key_(std::move(key)), name_(std::move(name)) {
    executable = QDir::toNativeSeparators(QFileInfo(executable).absoluteFilePath());
    dataRoot = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()));
    for (const auto& value : {executable, dataRoot})
        if (value.contains(u'"') || value.contains(QChar::Null) || value.contains(u'\n') ||
            value.contains(u'\r'))
            return;
    command_ = quote(executable) + QStringLiteral(" --data-dir ") + quote(dataRoot);
}
std::expected<StartupRegistration::Snapshot, QString> StartupRegistration::read() const {
    wil::unique_hkey key;
    auto result = RegOpenKeyExW(HKEY_CURRENT_USER, key_.toStdWString().c_str(), 0, KEY_QUERY_VALUE,
                                key.put());
    if (result == ERROR_FILE_NOT_FOUND)
        return Snapshot{};
    if (result != ERROR_SUCCESS)
        return std::unexpected(failure(result));
    DWORD type = 0, bytes = 0;
    const auto name = name_.toStdWString();
    result = RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND)
        return Snapshot{};
    if (result != ERROR_SUCCESS)
        return std::unexpected(failure(result));
    if (type != REG_SZ || bytes > 65536 || bytes % sizeof(wchar_t))
        return std::unexpected(QStringLiteral("MT 启动项格式不受支持，已保留原值"));
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, 0);
    result = RegQueryValueExW(key.get(), name.c_str(), nullptr, &type,
                              reinterpret_cast<BYTE*>(data.data()), &bytes);
    if (result != ERROR_SUCCESS || type != REG_SZ)
        return std::unexpected(failure(result));
    return Snapshot{QString::fromWCharArray(data.data())};
}
std::expected<void, QString> StartupRegistration::write(const Snapshot& value) const {
    wil::unique_hkey key;
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, key_.toStdWString().c_str(), 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, key.put(), nullptr);
    if (result != ERROR_SUCCESS)
        return std::unexpected(failure(result));
    const auto name = name_.toStdWString();
    if (value) {
        const auto data = value->toStdWString();
        result = RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(data.c_str()),
                                DWORD((data.size() + 1) * sizeof(wchar_t)));
    } else
        result = RegDeleteValueW(key.get(), name.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
        return std::unexpected(failure(result));
    auto actual = read();
    if (!actual)
        return std::unexpected(actual.error());
    if (*actual != value)
        return std::unexpected(QStringLiteral("MT 启动项回读不一致，请重新检查"));
    return {};
}
std::expected<void, QString> StartupRegistration::setEnabled(bool enabled) const {
    if (command_.isEmpty() || (enabled && command_.size() > 260))
        return std::unexpected(
            QStringLiteral("程序或数据路径无效/过长，请移至较短路径后启用开机启动"));
    auto before = read();
    if (!before)
        return std::unexpected(before.error());
    if (*before && **before != command_)
        return std::unexpected(
            QStringLiteral("MT 启动项属于另一安装位置或数据目录，已保留；请从原实例关闭后再启用"));
    return write(enabled ? Snapshot{command_} : Snapshot{});
}
std::expected<void, QString> StartupRegistration::restore(const Snapshot& before,
                                                          const Snapshot& expected) const {
    auto current = read();
    if (!current)
        return std::unexpected(current.error());
    if (*current != expected)
        return std::unexpected(QStringLiteral("启动项已被外部修改，未覆盖，请手动核对"));
    return write(before);
}
} // namespace mt
