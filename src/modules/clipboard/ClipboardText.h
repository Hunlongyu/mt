#pragma once
#include <QColor>
#include <QDateTime>
#include <QString>

namespace mt {
// Content-aware helpers shared by the list, preview and open action. All of them
// are pure: no file-system or network access, safe on the GUI thread.
enum class OpenKind { None, Url, Email, Path };
struct OpenTarget {
    OpenKind kind = OpenKind::None;
    QString value;
};
// Short colour tokens only (#rgb[a], #rrggbb[aa], rgb()/rgba(), bare 6/8 hex).
QColor clipColor(const QString& text);
OpenTarget openTarget(const QString& text);
// Characters as the user sees them: a surrogate pair counts once.
int clipCharacters(const QString& text);
int clipLines(const QString& text);
QString clipRelativeTime(const QDateTime& captured, const QDateTime& now);
} // namespace mt
