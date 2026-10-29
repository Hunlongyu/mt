#pragma once
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QJsonObject>
#include <QObject>
class QToolButton;
class QWidget;

namespace mt {
enum class Symbol {
    Menu,
    Pin,
    PinToTop,
    Close,
    Search,
    More,
    Text,
    Image,
    Files,
    Star,
    Settings,
    Back,
    Trash,
    Pause,
    Play,
    Sun,
    Moon,
    Layers,
    Copy,
    Open,
    Help,
    ChevronLeft,
    Palette, Grid, Gesture, MonitorOff, Info, Monitor, Plus, Pencil, Delete,
    Folder, Keyboard, Shield, Upload, Check,
    ChevronRight // keep last: the icon test iterates up to it
};
class Theme final : public QObject {
    Q_OBJECT
  public:
    // Shared header metrics: both main panels use one title/toolbar height so
    // the launcher and clipboard read as the same family of windows.
    static constexpr int headerHeight = 36;
    static constexpr int headerInset = 4;
    // Short UI motion; callers skip it when Windows client animations are off.
    static constexpr int motionMs = 140;
    QWidget* header(QWidget* parent) const;
    explicit Theme(QObject* parent = nullptr);
    void apply(const QString& name);
    QString name() const {
        return name_;
    }
    QColor color(const QString& role) const;
    QColor colorForTheme(const QString& name, const QString& role) const;
    QIcon icon(Symbol symbol, bool accent = false) const;
    // Lucide asset name backing a symbol (resources/icons/lucide/<name>.svg).
    static QString iconAsset(Symbol symbol);
    QToolButton* button(Symbol symbol, const QString& accessibleLabel, QWidget* parent = nullptr,
                        bool checkable = false);
  Q_SIGNALS:
    void changed();

  private:
    QString name_ = QStringLiteral("dark");
    QJsonObject document_;
    QJsonObject colors_;
    mutable QHash<int, QIcon> iconCache_;
};
} // namespace mt
