#include "Theme.h"
#include "PopupMenuStyle.h"
#include "ui/shell/SettingsControls.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QSvgRenderer>
#include <QHBoxLayout>
#include <QToolButton>
#include <algorithm>

namespace mt {
namespace {
QColor cssColor(const QString& value) {
    if (value.size() == 9) {
        QColor c(value.left(7));
        c.setAlpha(value.right(2).toInt(nullptr, 16));
        return c;
    }
    return QColor(value);
}
QColor over(QColor fg, QColor bg) {
    const double a = fg.alphaF();
    return QColor::fromRgbF(fg.redF() * a + bg.redF() * (1 - a),
                            fg.greenF() * a + bg.greenF() * (1 - a),
                            fg.blueF() * a + bg.blueF() * (1 - a));
}
} // namespace
Theme::Theme(QObject* parent) : QObject(parent) {
    QFile file(QStringLiteral(":/mt/theme.json"));
    if (file.open(QIODevice::ReadOnly))
        document_ = QJsonDocument::fromJson(file.readAll()).object();
    new PopupMenuStyle(*this);
}
QColor Theme::color(const QString& role) const {
    const auto product = document_.value(QStringLiteral("themes"))
                             .toObject()
                             .value(name_)
                             .toObject()
                             .value(QStringLiteral("product_colors"))
                             .toObject();
    if (product.contains(role))
        return cssColor(product.value(role).toString());
    const auto binding = document_.value(QStringLiteral("bindings")).toObject().value(role);
    if (binding.isString())
        return cssColor(colors_.value(binding.toString()).toString());
    if (binding.isObject()) {
        const auto b = binding.toObject();
        return over(
            cssColor(colors_.value(b.value(QStringLiteral("source")).toString()).toString()),
            cssColor(colors_.value(b.value(QStringLiteral("over")).toString()).toString()));
    }
    return QColor(Qt::magenta);
}
QColor Theme::colorForTheme(const QString& name, const QString& role) const {
    const auto theme = document_.value(QStringLiteral("themes")).toObject().value(name).toObject();
    const auto product = theme.value(QStringLiteral("product_colors")).toObject();
    if (product.contains(role)) return cssColor(product.value(role).toString());
    auto values = theme.value(QStringLiteral("colors")).toObject();
    const auto adaptations = theme.value(QStringLiteral("adaptations")).toObject();
    for (auto i = adaptations.begin(); i != adaptations.end(); ++i) values.insert(i.key(), i.value());
    const auto binding = document_.value(QStringLiteral("bindings")).toObject().value(role);
    if (binding.isString()) return cssColor(values.value(binding.toString()).toString());
    const auto b = binding.toObject();
    return over(cssColor(values.value(b.value(QStringLiteral("source")).toString()).toString()),
                cssColor(values.value(b.value(QStringLiteral("over")).toString()).toString()));
}
void Theme::apply(const QString& name) {
    iconCache_.clear();
    name_ = name == QStringLiteral("light") ? name : QStringLiteral("dark");
    colors_ = document_.value(QStringLiteral("themes"))
                  .toObject()
                  .value(name_)
                  .toObject()
                  .value(QStringLiteral("colors"))
                  .toObject();
    const auto adaptations = document_.value(QStringLiteral("themes"))
                                 .toObject()
                                 .value(name_)
                                 .toObject()
                                 .value(QStringLiteral("adaptations"))
                                 .toObject();
    for (auto i = adaptations.begin(); i != adaptations.end(); ++i)
        colors_.insert(i.key(), i.value());
    QPalette palette;
    palette.setColor(QPalette::Window, color(QStringLiteral("surface")));
    palette.setColor(QPalette::WindowText, color(QStringLiteral("text")));
    palette.setColor(QPalette::Base, color(QStringLiteral("surface")));
    palette.setColor(QPalette::AlternateBase, color(QStringLiteral("chrome")));
    palette.setColor(QPalette::Text, color(QStringLiteral("text")));
    palette.setColor(QPalette::Button, color(QStringLiteral("chrome")));
    palette.setColor(QPalette::ButtonText, color(QStringLiteral("text")));
    palette.setColor(QPalette::Highlight, color(QStringLiteral("surface-selected")));
    palette.setColor(QPalette::HighlightedText, color(QStringLiteral("text")));
    palette.setColor(QPalette::PlaceholderText, color(QStringLiteral("muted")));
    // Leave QToolTip's separate platform palette and default rendering untouched.
    palette.setColor(QPalette::Disabled, QPalette::Text, color(QStringLiteral("muted")));
    QApplication::setPalette(palette);
    QString qss = QStringLiteral(R"(
QWidget { color: @text; }
QWidget#PanelSurface, QDialog { background: @surface; }
QWidget#PanelSurface { border: none; }
QWidget#Toolbar { background: @chrome; border: none; border-bottom: 1px solid @border; }
QWidget#PanelTitle { color: @muted; }
QLabel { background: transparent; border: none; }
QLabel#Muted { color: @muted; }
QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 2px; background: transparent; }
QToolButton:pressed { background: @active-bg; }
QToolButton:hover { background: @surface-hover; }
QToolButton:checked { background: @button-bg; border-color: @button-bg; }
QToolButton:checked:hover { background: @button-hover; border-color: @button-hover; }
QToolButton#LauncherHeaderAction:hover { background: transparent; border-color: @border-strong; }
QToolButton#LauncherHeaderAction:pressed { background: @surface-selected; border-color: @focus; }
QToolButton#LauncherHeaderAction:checked, QToolButton#LauncherHeaderAction:checked:hover { background: @button-bg; border-color: @button-bg; }
QFrame#FilterGroup { background: @surface-subtle; border: 1px solid @border; border-radius: 7px; }
QToolButton#FilterButton { border-radius: 5px; }
QToolButton:focus, QPushButton:focus { border: 1px solid @focus; }
QPushButton { background: @chrome; border: 1px solid @border-strong; border-radius: 4px; padding: 6px 12px; }
QPushButton:hover { background: @surface-hover; }
QPushButton:default { background: @button-bg; color: @on-accent; border-color: @button-bg; }
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit { background: @field; border: 1px solid @field-border; border-radius: 4px; padding: 5px; selection-background-color: @surface-selected; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color: @focus; }
QListView, QTextBrowser { background: @surface; border: none; outline: none; }
QPlainTextEdit#PreviewText, QScrollArea#PreviewImage { background: transparent; border: none; padding: 0; }
QLabel#PreviewInfo { color: @muted; }
QListView::item:selected { background: @surface-selected; }
QMenu { background: @surface-raised; border: 1px solid @window-border; border-radius: 0; margin: 0; padding: 4px; font-size: 12px; }
QMenu::item { min-width: 132px; min-height: 30px; max-height: 30px; padding: 0 24px 0 10px; margin: 0; border: none; border-radius: 4px; }
QMenu::item:selected { background: @surface-hover; color: @text; }
QMenu::item:disabled { color: @faint; background: transparent; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
QWidget#BottomBar { background: @chrome; border: none; }
QTabBar::tab { background: transparent; border: none; border-bottom: 3px solid transparent; padding: 5px 8px; }
QTabBar::tab:selected { border-bottom-color: @tab-indicator; }
QTabBar::tab:hover { background: @surface-hover; }
QTabBar#LauncherCategories::tab { color: @muted; border-bottom-width: 2px; }
QTabBar#LauncherCategories::tab:selected { color: @tab-indicator; font-weight: 600; }
QTabBar#LauncherCategories::tab:hover { background: transparent; color: @text; }
QTabBar#LauncherCategories::tab:selected:hover { color: @tab-indicator; }
QListWidget#LauncherSearchResults::item { border-radius: 4px; padding: 4px 6px; }
QListWidget#LauncherSearchResults::item:hover { background: @surface-hover; }
QListWidget#LauncherSearchResults::item:selected { background: @surface-selected; }
QTabBar::scroller { width: 0px; }
QToolButton#TabScroll { border: none; border-radius: 0; background: transparent; padding: 0; }
QToolButton#TabScroll:hover { background: transparent; border: 1px solid @border-strong; border-radius: 4px; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 0; }
QScrollBar::handle:vertical { background: @scrollbar; border-radius: 2px; min-height: 24px; margin: 0 2px; }
QScrollBar::handle:vertical:hover { background: @muted; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QCheckBox { spacing: 8px; }
QGroupBox { border: 1px solid @border; border-radius: 4px; margin-top: 16px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; }
)");
    qss += settingsui::styleSheet();
    auto bindings = document_.value(QStringLiteral("bindings")).toObject().keys();
    std::sort(bindings.begin(), bindings.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const auto& role : bindings) {
        const auto c = color(role);
        qss.replace(u'@' + role, QStringLiteral("rgba(%1,%2,%3,%4)")
                                     .arg(c.red())
                                     .arg(c.green())
                                     .arg(c.blue())
                                     .arg(c.alpha()));
    }
    qApp->setStyleSheet(qss);
    Q_EMIT changed();
}
QString Theme::iconAsset(Symbol symbol) {
    // Every UI icon is a Lucide SVG (ISC, resources/icons/lucide/LICENSE.txt).
    // Add new icons from Lucide; see resources/icons/lucide/README.md.
    switch (symbol) {
    case Symbol::Menu:
        return QStringLiteral("menu");
    case Symbol::Pin:
        return QStringLiteral("pin");
    case Symbol::PinToTop:
        return QStringLiteral("arrow-up-to-line");
    case Symbol::Close:
        return QStringLiteral("x");
    case Symbol::Search:
        return QStringLiteral("search");
    case Symbol::More:
        return QStringLiteral("ellipsis");
    case Symbol::Text:
        return QStringLiteral("text");
    case Symbol::Image:
        return QStringLiteral("image");
    case Symbol::Files:
        return QStringLiteral("file-text");
    case Symbol::Star:
        return QStringLiteral("star");
    case Symbol::Settings:
        return QStringLiteral("settings");
    case Symbol::Back:
        return QStringLiteral("arrow-left");
    case Symbol::Trash:
        return QStringLiteral("circle-x");
    case Symbol::Pause:
        return QStringLiteral("pause");
    case Symbol::Play:
        return QStringLiteral("play");
    case Symbol::Sun:
        return QStringLiteral("sun");
    case Symbol::Moon:
        return QStringLiteral("moon");
    case Symbol::Layers:
        return QStringLiteral("layers");
    case Symbol::Copy:
        return QStringLiteral("copy");
    case Symbol::Open:
        return QStringLiteral("external-link");
    case Symbol::Help:
        return QStringLiteral("circle-help");
    case Symbol::ChevronLeft:
        return QStringLiteral("chevron-left");
    case Symbol::Palette: return QStringLiteral("palette");
    case Symbol::Grid: return QStringLiteral("layout-grid");
    case Symbol::Gesture: return QStringLiteral("mouse-pointer-2");
    case Symbol::MonitorOff: return QStringLiteral("monitor-off");
    case Symbol::Info: return QStringLiteral("info");
    case Symbol::Monitor: return QStringLiteral("monitor");
    case Symbol::Plus: return QStringLiteral("plus");
    case Symbol::Pencil: return QStringLiteral("pencil");
    case Symbol::Delete: return QStringLiteral("trash-2");
    case Symbol::Folder: return QStringLiteral("folder");
    case Symbol::Keyboard: return QStringLiteral("keyboard");
    case Symbol::Shield: return QStringLiteral("shield-check");
    case Symbol::Upload: return QStringLiteral("upload");
    case Symbol::Check: return QStringLiteral("check");
    case Symbol::ChevronRight:
        return QStringLiteral("chevron-right");
    }
    return {};
}
QIcon Theme::icon(Symbol symbol, bool accent) const {
    const int cacheKey = static_cast<int>(symbol) * 2 + (accent ? 1 : 0);
    if (iconCache_.contains(cacheKey))
        return iconCache_.value(cacheKey);
    QSvgRenderer renderer(QStringLiteral(":/mt/icons/lucide/%1.svg").arg(iconAsset(symbol)));
    QIcon result;
    if (renderer.isValid()) {
        for (int size : {16, 18, 20, 24, 32}) {
            for (qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
                for (auto mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
                    for (auto state : {QIcon::Off, QIcon::On}) {
                        QPixmap raster(qRound(size * scale), qRound(size * scale));
                        raster.setDevicePixelRatio(scale);
                        raster.fill(Qt::transparent);
                        QPainter painter(&raster);
                        painter.setRenderHint(QPainter::Antialiasing);
                        renderer.render(&painter, QRectF(0, 0, size, size));
                        // Lucide strokes use currentColor; tint to the shared theme role.
                        auto tint = color(accent                  ? QStringLiteral("accent")
                                          : state == QIcon::On    ? QStringLiteral("on-accent")
                                          : mode == QIcon::Active ? QStringLiteral("text")
                                                                  : QStringLiteral("icon"));
                        if (mode == QIcon::Disabled)
                            tint.setAlphaF(0.4F);
                        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                        painter.fillRect(QRectF(0, 0, size, size), tint);
                        painter.end();
                        result.addPixmap(raster, mode, state);
                    }
                }
            }
        }
    }
    iconCache_.insert(cacheKey, result);
    return result;
}
QWidget* Theme::header(QWidget* parent) const {
    auto* header = new QWidget(parent);
    header->setObjectName(QStringLiteral("Toolbar"));
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(headerHeight);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(headerInset + 2, headerInset, headerInset + 2, headerInset);
    row->setSpacing(2);
    return header;
}
QToolButton* Theme::button(Symbol symbol, const QString& label, QWidget* parent, bool checkable) {
    auto* button = new QToolButton(parent);
    button->setFixedSize(28, 28);
    button->setIcon(icon(symbol));
    button->setIconSize(QSize(18, 18));
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setCheckable(checkable);
    // Header actions all have panel shortcuts; taking focus only drew a stray
    // focus ring on the first button whenever a panel was activated.
    button->setFocusPolicy(Qt::NoFocus);
    connect(this, &Theme::changed, button,
            [this, button, symbol] { button->setIcon(icon(symbol)); });
    return button;
}
} // namespace mt
