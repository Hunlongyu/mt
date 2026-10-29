#include "GestureRules.h"
#include "infrastructure/i18n/I18n.h"
#include <QKeySequence>
#include <QRegularExpression>

namespace mt {
QStringList gestureCommands() {
    return {QStringLiteral("close_window"), QStringLiteral("minimize"),
            QStringLiteral("maximize"), QStringLiteral("restore"),
            QStringLiteral("toggle_maximize"), QStringLiteral("scroll_top"),
            QStringLiteral("scroll_bottom"), QStringLiteral("volume_up"),
            QStringLiteral("volume_down"), QStringLiteral("volume_mute"),
            QStringLiteral("media_play"), QStringLiteral("copy"), QStringLiteral("paste"),
            QStringLiteral("open_exe_dir"), QStringLiteral("none")};
}
QString gestureCommandLabel(const QString& command) {
    const QStringList labels{tx(u"关闭窗口"), tx(u"最小化"), tx(u"最大化"), tx(u"还原窗口"),
        tx(u"最大化/还原"), tx(u"滚动到顶部"), tx(u"滚动到底部"), tx(u"增大音量"),
        tx(u"减小音量"), tx(u"静音"), tx(u"播放 / 暂停"), tx(u"复制"), tx(u"粘贴"),
        tx(u"打开程序所在文件夹"), tx(u"无动作")};
    const auto index = gestureCommands().indexOf(command);
    return index >= 0 ? labels[index] : command;
}
bool gestureKeySupported(int key) {
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9) ||
        (key >= Qt::Key_F1 && key <= Qt::Key_F24))
        return true;
    switch (key) {
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Tab: case Qt::Key_Escape:
    case Qt::Key_Space: case Qt::Key_Delete: case Qt::Key_Insert: case Qt::Key_Backspace:
        return true;
    default: return false;
    }
}
std::expected<GestureAction, QString> GestureAction::parse(const QString& text) {
    const auto colon = text.indexOf(u':');
    const auto type = text.left(colon).toLower();
    auto value = text.mid(colon + 1).trimmed();
    if (colon <= 0 || value.isEmpty() || value.size() > 4096)
        return std::unexpected(tx(u"手势动作配置格式无效"));
    if (type == QStringLiteral("cmd")) {
        value = value.toLower();
        // Both the existing mt action names and the descriptive command names
        // denote the same operations; no file or data migration is performed.
        if (value == QStringLiteral("close")) value = QStringLiteral("close_window");
        if (value == QStringLiteral("directory")) value = QStringLiteral("open_exe_dir");
        if (!gestureCommands().contains(value))
            return std::unexpected(tx(u"未知手势动作：%1").arg(value));
        return GestureAction{Kind::Command, value};
    }
    if (type == QStringLiteral("run"))
        return GestureAction{Kind::Open, value};
    if (type == QStringLiteral("key")) {
        const auto keys = QKeySequence::fromString(value, QKeySequence::PortableText);
        if (keys.count() != 1 || !gestureKeySupported(keys[0].key()) ||
            (keys[0].keyboardModifiers() & (Qt::KeypadModifier | Qt::GroupSwitchModifier)))
            return std::unexpected(tx(u"暂不支持此快捷键：%1").arg(value));
        return GestureAction{Kind::Shortcut, keys.toString(QKeySequence::PortableText)};
    }
    return std::unexpected(tx(u"手势动作配置格式无效"));
}
QString GestureAction::encode() const {
    return (kind == Kind::Command ? QStringLiteral("cmd:") :
            kind == Kind::Shortcut ? QStringLiteral("key:") : QStringLiteral("run:")) + value;
}
QString GestureAction::label() const {
    if (kind == Kind::Command)
        return gestureCommandLabel(value);
    if (kind == Kind::Open) return tx(u"运行程序");
    const QMap<QString, QString> names{{QStringLiteral("Alt+Left"), tx(u"后退")},
        {QStringLiteral("Alt+Right"), tx(u"前进")}, {QStringLiteral("F5"), tx(u"刷新")},
        {QStringLiteral("Ctrl+W"), tx(u"关闭标签页")}, {QStringLiteral("Ctrl+T"), tx(u"新建标签页")},
        {QStringLiteral("Del"), tx(u"删除")}};
    return names.value(value, value);
}
std::expected<QString, QString> normalizeGesture(const QString& input) {
    QString out;
    for (const auto c : input.trimmed()) {
        if (!QStringLiteral("12346789").contains(c))
            return std::unexpected(tx(u"方向只能使用小键盘数字 1–9（不含 5）"));
        if (out.isEmpty() || out.back() != c)
            out += c;
    }
    if (out.isEmpty() || out.size() > 15)
        return std::unexpected(tx(u"手势方向长度须为 1–15"));
    return out;
}
QString gestureArrows(const QString& directions) {
    const QString digits = QStringLiteral("12346789");
    const QString arrows = QStringLiteral("↙↓↘←→↖↑↗");
    QString out;
    for (auto c : directions) {
        const auto index = digits.indexOf(c);
        if (index >= 0) out += arrows[index];
    }
    return out;
}
namespace {
bool validProcess(const QString& process) {
    return !process.trimmed().isEmpty() && process.size() <= 260 &&
           !process.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
}
std::expected<QMap<QString, GestureAction>, QString> parseMap(const QJsonObject& object, int limit) {
    if (object.size() > limit)
        return std::unexpected(tx(u"手势规则数量超出上限"));
    QMap<QString, GestureAction> result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        auto key = normalizeGesture(it.key());
        if (!key) return std::unexpected(key.error());
        if (!it.value().isString()) return std::unexpected(tx(u"手势动作配置格式无效"));
        auto action = GestureAction::parse(it.value().toString());
        if (!action) return std::unexpected(action.error());
        if (result.contains(*key)) return std::unexpected(tx(u"同一程序内的手势方向不能重复"));
        result.insert(*key, *action);
    }
    return result;
}
} // namespace
std::expected<GestureRules, QString> GestureRules::parse(const QJsonObject& globalObject,
    const QJsonObject& appObject, QStringList block, QStringList allow) {
    GestureRules rules;
    auto global = parseMap(globalObject, 128);
    if (!global) return std::unexpected(global.error());
    rules.global = *global;
    if (appObject.size() > 64 || block.size() > 64 || allow.size() > 64)
        return std::unexpected(tx(u"进程名单最多 64 项"));
    for (auto* list : {&block, &allow})
        for (auto& process : *list) {
            if (!validProcess(process)) return std::unexpected(tx(u"进程名单只能填写程序文件名，如 game.exe"));
            process = process.trimmed().toCaseFolded();
        }
    rules.excluded = block;
    rules.allowed = allow;
    for (auto it = appObject.begin(); it != appObject.end(); ++it) {
        if (!validProcess(it.key()) || !it.value().isObject())
            return std::unexpected(tx(u"手势程序规则格式无效"));
        auto process = it.key().trimmed().toCaseFolded();
        if (rules.applications.contains(process))
            return std::unexpected(tx(u"同一程序内的手势方向不能重复"));
        auto actions = parseMap(it.value().toObject(), 64);
        if (!actions) return std::unexpected(actions.error());
        rules.applications.insert(process, *actions);
    }
    return rules;
}
bool GestureRules::permits(const QString& process) const {
    const auto name = process.toCaseFolded();
    return !name.isEmpty() && !excluded.contains(name) && (allowed.isEmpty() || allowed.contains(name));
}
QMap<QString, GestureAction> GestureRules::forProcess(const QString& process) const {
    auto result = global;
    const auto overrides = applications.value(process.toCaseFolded());
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        result.insert(it.key(), it.value()); // cmd:none is an override, not fallback
    return result;
}
} // namespace mt
