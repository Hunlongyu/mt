#pragma once
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <expected>

namespace mt {
struct GestureAction {
    enum class Kind { Command, Shortcut, Open };
    Kind kind = Kind::Command;
    QString value;
    QString encode() const;
    QString label() const;
    static std::expected<GestureAction, QString> parse(const QString& text);
};
// Explicit, ordered key pairs; platform code only translates these Qt keys to
// native input. Validation and execution share exactly the same accepted set.
bool gestureKeySupported(int qtKey);
QStringList gestureCommands();
QString gestureCommandLabel(const QString& command);
QString gestureArrows(const QString& directions);
std::expected<QString, QString> normalizeGesture(const QString& directions);

struct GestureRules {
    QMap<QString, GestureAction> global;
    QMap<QString, QMap<QString, GestureAction>> applications;
    QStringList excluded;
    QStringList allowed;
    static std::expected<GestureRules, QString> parse(const QJsonObject& global,
                                                     const QJsonObject& applications,
                                                     QStringList excluded = {},
                                                     QStringList allowed = {});
    bool permits(const QString& process) const;
    QMap<QString, GestureAction> forProcess(const QString& process) const;
};
} // namespace mt
