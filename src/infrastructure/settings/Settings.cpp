#include "Settings.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include "infrastructure/i18n/I18n.h"
#include "modules/launcher/EdgeTrigger.h"
#include "modules/launcher/LauncherCatalog.h"
#include "modules/gestures/GestureRules.h"
#include <algorithm>
#include <cmath>

namespace mt {
Settings Settings::defaults() {
    Settings s;
    s.gestureActions = {{QStringLiteral("1"), QStringLiteral("cmd:minimize")},
                        {QStringLiteral("2"), QStringLiteral("cmd:scroll_bottom")},
                        {QStringLiteral("3"), QStringLiteral("key:Delete")},
                        {QStringLiteral("4"), QStringLiteral("key:Alt+Left")},
                        {QStringLiteral("6"), QStringLiteral("key:Alt+Right")},
                        {QStringLiteral("7"), QStringLiteral("key:Esc")},
                        {QStringLiteral("8"), QStringLiteral("cmd:scroll_top")},
                        {QStringLiteral("9"), QStringLiteral("cmd:toggle_maximize")},
                        {QStringLiteral("26"), QStringLiteral("cmd:close_window")},
                        {QStringLiteral("39"), QStringLiteral("key:F5")},
                        {QStringLiteral("86"), QStringLiteral("cmd:open_exe_dir")}};
    const QJsonObject browser{{QStringLiteral("26"), QStringLiteral("key:Ctrl+W")}};
    s.gestureApplicationActions = {{QStringLiteral("chrome.exe"), browser},
                                  {QStringLiteral("msedge.exe"), browser}};
    return s;
}
std::expected<void, QString> Settings::validate() const {
    const auto oneOf = [](const QString& value, std::initializer_list<const char*> allowed) {
        return std::ranges::any_of(allowed,
                                   [&](const char* a) { return value == QLatin1StringView(a); });
    };
    if (!oneOf(theme, {"light", "dark", "auto"}))
        return std::unexpected(tx(u"主题值无效"));
    if (historyLimit < 1 || historyLimit > 100000)
        return std::unexpected(tx(u"显示条数必须为 1–100000"));
    if (storageLimit < historyLimit || storageLimit > 100000)
        return std::unexpected(tx(u"存储上限必须不小于显示条数且不超过 100000"));
    if (!oneOf(previewSide, {"left", "right"}) || !oneOf(clipboardPlacement, {"cursor", "center"}) ||
        !oneOf(openMouseButton, {"middle", "right", "none"}))
        return std::unexpected(tx(u"剪贴板预览、位置或打开方式值无效"));
    // Single letters only; D/F/T/S/H already act on the selected record.
    if (!openKey.isEmpty() &&
        (openKey.size() != 1 || openKey[0] < u'A' || openKey[0] > u'Z' ||
         QStringLiteral("DFTSH").contains(openKey[0])))
        return std::unexpected(tx(u"打开按键须为未被占用的单个字母"));
    if (launcherOpacity < 30 || launcherOpacity > 100 || clipboardOpacity < 60 ||
        clipboardOpacity > 100)
        return std::unexpected(tx(u"启动器不透明度须为 30–100%，剪贴板须为 60–100%"));
    if (gestureThreshold < 5 || gestureThreshold > 200 || (gestureTimeout != 0 && gestureTimeout < 200) ||
        gestureTimeout > 10000)
        return std::unexpected(tx(u"手势阈值或超时无效"));
    if (!oneOf(gestureTrigger, {"right", "middle", "x1", "x2"}))
        return std::unexpected(tx(u"手势触发键无效"));
    if (auto rules = GestureRules::parse(gestureActions, gestureApplicationActions,
                                         excludedProcesses, gestureAllowlist); !rules)
        return std::unexpected(rules.error());
    if (launcherColumns < 3 || launcherColumns > 20 || launcherRows < 3 || launcherRows > 20)
        return std::unexpected(tx(u"启动面板行数和列数须为 3–20"));
    if (!oneOf(launcherEdgeMode, {"outer", "every"}))
        return std::unexpected(tx(u"屏幕边缘唤起设置无效"));
    for (const auto& zone : launcherEdgeZones)
        if (edgeZoneFromKey(zone) == EdgeZone::None)
            return std::unexpected(tx(u"屏幕边缘唤起设置无效"));
    // Executable names only: no folders, wildcards or more than 64 entries.
    for (const auto* list : {&launcherEdgeBlocklist, &launcherEdgeAllowlist}) {
        if (list->size() > 64)
            return std::unexpected(tx(u"进程名单最多 64 项"));
        for (const auto& name : *list)
            if (name.trimmed().isEmpty() || name.size() > 260 ||
                name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
                return std::unexpected(tx(u"进程名单只能填写程序文件名，如 game.exe"));
    }
    if (requirePassword && protectedPassword.isEmpty())
        return std::unexpected(tx(u"请先设置有效的解锁密码"));
    QSet<QString> keys;
    const QList<QPair<bool, QString>> shortcuts{{clipboardEnabled, clipboardHotkey},
                                                {launcherEnabled, launcherHotkey},
                                                {lockEnabled, lockHotkey}};
    for (const auto& [enabled, key] : shortcuts) {
        if (!enabled)
            continue;
        const auto normalized = key.toCaseFolded().remove(u' ');
        if (normalized.isEmpty() || keys.contains(normalized))
            return std::unexpected(tx(u"快捷键不能为空或相互重复"));
        keys.insert(normalized);
    }
    return {};
}
std::expected<Settings, QString> Settings::decode(const QByteArray& data) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(
            tx(u"配置解析失败，原文件已保留：%1").arg(error.errorString()));
    const auto o = doc.object();
    if (o.value(QStringLiteral("schema")).toInt() != 1)
        return std::unexpected(tx(u"不支持此配置版本，原文件已保留"));
    const QStringList strings{QStringLiteral("theme"),           QStringLiteral("clipboardHotkey"),
                              QStringLiteral("launcherHotkey"),  QStringLiteral("lockHotkey"),
                              QStringLiteral("protectedPassword"),
                              QStringLiteral("previewSide"),     QStringLiteral("clipboardPlacement"),
                              QStringLiteral("openKey"),         QStringLiteral("openMouseButton"),
                              QStringLiteral("launcherEdgeMode"), QStringLiteral("gestureTrigger")};
    for (const auto& key : strings)
        if (o.contains(key) && !o[key].isString())
            return std::unexpected(tx(u"配置类型错误：%1").arg(key));
    const QStringList integers{QStringLiteral("historyLimit"),    QStringLiteral("gestureThreshold"),
                               QStringLiteral("gestureTimeout"),  QStringLiteral("launcherOpacity"),
                               QStringLiteral("clipboardOpacity"), QStringLiteral("storageLimit"),
                               QStringLiteral("launcherColumns"), QStringLiteral("launcherRows")};
    for (const auto& key : integers)
        if (o.contains(key) &&
            (!o[key].isDouble() || std::floor(o[key].toDouble()) != o[key].toDouble() ||
             o[key].toDouble() > 100000 || o[key].toDouble() < 0))
            return std::unexpected(tx(u"配置数值无效：%1").arg(key));
    auto s = defaults();
    s.extra = o;
    // Reject malformed present values instead of silently resetting user settings.
    const QStringList bools{
        QStringLiteral("startHidden"), QStringLiteral("autoCheckUpdates"), QStringLiteral("clipboardEnabled"), QStringLiteral("launcherEnabled"),
        QStringLiteral("gesturesEnabled"),  QStringLiteral("lockEnabled"), QStringLiteral("lockPaused"),
        QStringLiteral("clipboardPaused"),  QStringLiteral("maskSensitive"),
        QStringLiteral("launcherPinned"),   QStringLiteral("requirePassword"),
        QStringLiteral("gestureTrail"),     QStringLiteral("disableGesturesFullscreen"),
        QStringLiteral("gestureShowName"),
        QStringLiteral("pasteOnConfirm"),   QStringLiteral("clipboardSuppressFullscreen"),
        QStringLiteral("hoverPreview"),     QStringLiteral("spacePreview"),
        QStringLiteral("launcherEdgeEnabled"), QStringLiteral("launcherEdgeSuppressFullscreen")};
    for (const auto& k : bools)
        if (o.contains(k) && !o[k].isBool())
            return std::unexpected(tx(u"配置类型错误：%1").arg(k));
#define LOAD_BOOL(name) s.name = o.value(QStringLiteral(#name)).toBool(s.name)
#define LOAD_INT(name) s.name = o.value(QStringLiteral(#name)).toInt(s.name)
#define LOAD_TEXT(name) s.name = o.value(QStringLiteral(#name)).toString(s.name)
    LOAD_BOOL(startHidden);
    LOAD_BOOL(autoCheckUpdates);
    LOAD_BOOL(clipboardEnabled);
    LOAD_BOOL(launcherEnabled);
    LOAD_BOOL(gesturesEnabled);
    LOAD_BOOL(lockEnabled);
    LOAD_BOOL(lockPaused);
    LOAD_BOOL(clipboardPaused);
    LOAD_BOOL(maskSensitive);
    LOAD_BOOL(launcherPinned);
    LOAD_BOOL(requirePassword);
    LOAD_BOOL(gestureTrail);
    LOAD_BOOL(gestureShowName);
    LOAD_TEXT(gestureTrigger);
    LOAD_BOOL(disableGesturesFullscreen);
    LOAD_BOOL(pasteOnConfirm);
    LOAD_BOOL(clipboardSuppressFullscreen);
    LOAD_BOOL(hoverPreview);
    LOAD_BOOL(spacePreview);
    LOAD_BOOL(launcherEdgeEnabled);
    LOAD_BOOL(launcherEdgeSuppressFullscreen);
    LOAD_TEXT(launcherEdgeMode);
    LOAD_INT(historyLimit);
    // Before storageLimit existed, historyLimit was also the pruning bound. Never
    // let an upgrade prune more than that setting used to keep.
    s.storageLimit = std::max(s.storageLimit, s.historyLimit);
    LOAD_INT(storageLimit);
    LOAD_TEXT(previewSide);
    LOAD_TEXT(clipboardPlacement);
    LOAD_TEXT(openKey);
    LOAD_TEXT(openMouseButton);
    LOAD_INT(launcherOpacity);
    LOAD_INT(launcherColumns);
    LOAD_INT(launcherRows);
    LOAD_INT(clipboardOpacity);
    LOAD_INT(gestureThreshold);
    LOAD_INT(gestureTimeout);
    LOAD_TEXT(theme);
    LOAD_TEXT(clipboardHotkey);
    LOAD_TEXT(launcherHotkey);
    LOAD_TEXT(lockHotkey);
    LOAD_TEXT(protectedPassword);
#undef LOAD_BOOL
#undef LOAD_INT
#undef LOAD_TEXT
    if (o.contains(QStringLiteral("gestureActions"))) {
        if (!o[QStringLiteral("gestureActions")].isObject())
            return std::unexpected(tx(u"手势动作配置格式无效"));
        s.gestureActions = o[QStringLiteral("gestureActions")].toObject();
    }
    if (o.contains(QStringLiteral("gestureApplicationActions"))) {
        if (!o[QStringLiteral("gestureApplicationActions")].isObject())
            return std::unexpected(tx(u"手势程序规则格式无效"));
        s.gestureApplicationActions = o[QStringLiteral("gestureApplicationActions")].toObject();
    }
    const auto stringList = [&o](const QString& key, QStringList& out) -> bool {
        if (!o.contains(key))
            return true;
        if (!o[key].isArray())
            return false;
        out.clear();
        for (const auto& v : o[key].toArray()) {
            if (!v.isString())
                return false;
            out.append(v.toString());
        }
        return true;
    };
    if (!stringList(QStringLiteral("excludedProcesses"), s.excludedProcesses) ||
        !stringList(QStringLiteral("gestureAllowlist"), s.gestureAllowlist))
        return std::unexpected(tx(u"手势程序规则格式无效"));
    if (!stringList(QStringLiteral("launcherEdgeZones"), s.launcherEdgeZones) ||
        !stringList(QStringLiteral("launcherEdgeBlocklist"), s.launcherEdgeBlocklist) ||
        !stringList(QStringLiteral("launcherEdgeAllowlist"), s.launcherEdgeAllowlist))
        return std::unexpected(tx(u"屏幕边缘唤起设置无效"));
    if (const auto check = s.validate(); !check)
        return std::unexpected(check.error());
    return s;
}
QByteArray Settings::encode() const {
    auto o = extra;
    o.remove(QStringLiteral("language")); // The application has one Chinese interface.
    o.insert(QStringLiteral("schema"), 1);
#define SAVE(name) o.insert(QStringLiteral(#name), name)
    SAVE(theme);
    SAVE(clipboardEnabled);
    SAVE(launcherEnabled);
    SAVE(gesturesEnabled);
    SAVE(lockEnabled);
    SAVE(lockPaused);
    SAVE(clipboardPaused);
    SAVE(maskSensitive);
    SAVE(startHidden);
    SAVE(autoCheckUpdates);
    SAVE(historyLimit);
    SAVE(storageLimit);
    SAVE(pasteOnConfirm);
    SAVE(clipboardSuppressFullscreen);
    SAVE(hoverPreview);
    SAVE(spacePreview);
    SAVE(previewSide);
    SAVE(clipboardPlacement);
    SAVE(openKey);
    SAVE(openMouseButton);
    SAVE(launcherOpacity);
    SAVE(clipboardOpacity);
    SAVE(clipboardHotkey);
    SAVE(launcherHotkey);
    SAVE(lockHotkey);
    SAVE(launcherPinned);
    SAVE(launcherColumns);
    SAVE(launcherRows);
    SAVE(launcherEdgeEnabled);
    SAVE(launcherEdgeMode);
    SAVE(launcherEdgeSuppressFullscreen);
    SAVE(requirePassword);
    SAVE(protectedPassword);
    SAVE(gestureThreshold);
    SAVE(gestureTimeout);
    SAVE(gestureTrail);
    SAVE(gestureShowName);
    SAVE(gestureTrigger);
    SAVE(disableGesturesFullscreen);
    SAVE(gestureActions);
    SAVE(gestureApplicationActions);
#undef SAVE
    o.insert(QStringLiteral("excludedProcesses"), QJsonArray::fromStringList(excludedProcesses));
    o.insert(QStringLiteral("gestureAllowlist"), QJsonArray::fromStringList(gestureAllowlist));
    o.insert(QStringLiteral("launcherEdgeZones"), QJsonArray::fromStringList(launcherEdgeZones));
    o.insert(QStringLiteral("launcherEdgeBlocklist"),
             QJsonArray::fromStringList(launcherEdgeBlocklist));
    o.insert(QStringLiteral("launcherEdgeAllowlist"),
             QJsonArray::fromStringList(launcherEdgeAllowlist));
    return QJsonDocument(o).toJson();
}
SettingsStore::SettingsStore(QString root) : root_(std::move(root)) {}
std::expected<Settings, QString> SettingsStore::load() {
    if (auto result = recoverLauncherConfiguration(); !result) {
        writable_ = false;
        return std::unexpected(result.error());
    }
    QFile f(QDir(root_).filePath(QStringLiteral("settings.json")));
    if (!f.exists())
        return Settings::defaults();
    if (!f.open(QIODevice::ReadOnly)) {
        writable_ = false;
        return std::unexpected(f.errorString());
    }
    if (f.size() > 2 * 1024 * 1024) {
        writable_ = false;
        return std::unexpected(tx(u"配置文件异常大，已保留"));
    }
    auto result = Settings::decode(f.readAll());
    writable_ = result.has_value();
    return result;
}
std::expected<void, QString> SettingsStore::save(const Settings& s) const {
    if (!writable_)
        return std::unexpected(tx(u"配置加载失败，本次运行禁止覆盖原文件"));
    if (auto result = s.validate(); !result)
        return result;
    if (!QDir().mkpath(root_))
        return std::unexpected(tx(u"无法创建数据目录"));
    QSaveFile f(QDir(root_).filePath(QStringLiteral("settings.json")));
    const auto data = s.encode();
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        return std::unexpected(f.errorString());
    return {};
}
std::expected<void, QString> SettingsStore::recoverLauncherConfiguration() const {
    const QDir root(root_);
    QFile journal(root.filePath(QStringLiteral("launcher-settings-rollback.json")));
    if (!journal.exists())
        return {};
    if (!journal.open(QIODevice::ReadOnly) || journal.size() > 32 * 1024 * 1024)
        return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(journal.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("schema")).toInt() != 1)
        return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
    const auto object = document.object();
    journal.close();
    // Only these two fixed filenames are allowed; the journal cannot select paths.
    for (const auto& name : {QStringLiteral("launcher.json"), QStringLiteral("settings.json")}) {
        const auto entry = object.value(name).toObject();
        if (!entry.value(QStringLiteral("existed")).isBool() ||
            !entry.value(QStringLiteral("bytes")).isString())
            return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
        const auto path = root.filePath(name);
        if (entry.value(QStringLiteral("existed")).toBool()) {
            const auto bytes = QByteArray::fromBase64Encoding(
                entry.value(QStringLiteral("bytes")).toString().toLatin1(),
                QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes)
                return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(*bytes) != (*bytes).size() || !file.commit())
                return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
        } else if (QFile::exists(path) && !QFile::remove(path)) {
            return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
        }
    }
    if (!journal.remove())
        return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
    return {};
}
std::expected<void, QString> SettingsStore::saveLauncherConfiguration(
    const Settings& settings, const QString& catalogPath, const LauncherCatalog& before,
    const LauncherCatalog& after) const {
    Q_UNUSED(before);
    if (!writable_)
        return std::unexpected(tx(u"配置加载失败，本次运行禁止覆盖原文件"));
    if (auto result = settings.validate(); !result) return result;
    if (auto result = after.validate(); !result) return result;
    const QDir root(root_);
    if (QFileInfo(catalogPath).absoluteFilePath() !=
        QFileInfo(root.filePath(QStringLiteral("launcher.json"))).absoluteFilePath())
        return std::unexpected(tx(u"配置事务尚未恢复，禁止继续写入"));
    if (!QDir().mkpath(root_))
        return std::unexpected(tx(u"无法创建数据目录"));
    QJsonObject rollback{{QStringLiteral("schema"), 1}};
    for (const auto& name : {QStringLiteral("launcher.json"), QStringLiteral("settings.json")}) {
        QFile file(root.filePath(name));
        const bool existed = file.exists();
        if (existed && (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024))
            return std::unexpected(file.errorString());
        rollback.insert(name, QJsonObject{{QStringLiteral("existed"), existed},
            {QStringLiteral("bytes"), QString::fromLatin1(existed ? file.readAll().toBase64() : QByteArray{})}});
    }
    // Stage both replacements before changing either destination.
    QSaveFile catalogFile(catalogPath), settingsFile(root.filePath(QStringLiteral("settings.json")));
    const auto catalogBytes = after.encode();
    const auto settingsBytes = settings.encode();
    if (!catalogFile.open(QIODevice::WriteOnly) || catalogFile.write(catalogBytes) != catalogBytes.size())
        return std::unexpected(catalogFile.errorString());
    if (!settingsFile.open(QIODevice::WriteOnly) || settingsFile.write(settingsBytes) != settingsBytes.size())
        return std::unexpected(settingsFile.errorString());
    const auto journalPath = root.filePath(QStringLiteral("launcher-settings-rollback.json"));
    QSaveFile journal(journalPath);
    const auto bytes = QJsonDocument(rollback).toJson(QJsonDocument::Compact);
    if (!journal.open(QIODevice::WriteOnly) || journal.write(bytes) != bytes.size() || !journal.commit())
        return std::unexpected(journal.errorString());
    QString failure;
    if (!catalogFile.commit()) failure = catalogFile.errorString();
    else if (!settingsFile.commit()) failure = settingsFile.errorString();
    else if (!QFile::remove(journalPath)) failure = tx(u"配置事务尚未恢复，禁止继续写入");
    else return {};
    catalogFile.cancelWriting();
    settingsFile.cancelWriting();
    if (auto restored = recoverLauncherConfiguration(); !restored)
        return restored;
    return std::unexpected(failure);
}
} // namespace mt
