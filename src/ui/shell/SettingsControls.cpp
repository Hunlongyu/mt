#include "SettingsControls.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QTextDocument>

namespace mt::settingsui {
namespace {
void paintChevron(QPainter& painter, const Theme& theme, const QRect& area, int size,
                  qreal rotation, bool enabled) {
    // Rotate the existing Lucide chevron asset, keeping theme tint and DPI handling.
    painter.save();
    painter.translate(QRectF(area).center());
    painter.rotate(rotation);
    theme.icon(Symbol::ChevronRight)
        .paint(&painter, QRect(-size / 2, -size / 2, size, size), Qt::AlignCenter,
               enabled ? QIcon::Normal : QIcon::Disabled);
    painter.restore();
}
} // namespace
ComboBox::ComboBox(Theme& theme, QWidget* parent) : QComboBox(parent), theme_(theme) {
    setProperty("settingsControl", true);
    setFixedHeight(34);
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void ComboBox::paintEvent(QPaintEvent* event) {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    QPainter painter(this);
    paintChevron(painter, theme_,
                 style()->subControlRect(QStyle::CC_ComboBox, &option,
                                         QStyle::SC_ComboBoxArrow, this),
                 16, 90, isEnabled());
}
SpinBox::SpinBox(Theme& theme, QWidget* parent) : QSpinBox(parent), theme_(theme) {
    setProperty("settingsControl", true);
    setFixedHeight(34);
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void SpinBox::paintEvent(QPaintEvent* event) {
    QSpinBox::paintEvent(event);
    if (buttonSymbols() == QAbstractSpinBox::NoButtons)
        return;
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter painter(this);
    const auto paintStep = [&](QStyle::SubControl control, StepEnabledFlag step,
                               qreal rotation) {
        paintChevron(painter, theme_,
                     style()->subControlRect(QStyle::CC_SpinBox, &option, control, this),
                     12, rotation,
                     isEnabled() && !isReadOnly() && option.stepEnabled.testFlag(step));
    };
    paintStep(QStyle::SC_SpinBoxUp, StepUpEnabled, -90);
    paintStep(QStyle::SC_SpinBoxDown, StepDownEnabled, 90);
}
Switch::Switch(Theme& theme, const QString& label, QWidget* parent)
    : QCheckBox(parent), theme_(theme) {
    setAccessibleName(label);
    setToolTip(label);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void Switch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.45);
    p.setPen(QPen(
        theme_.color(isChecked() ? QStringLiteral("button-bg") : QStringLiteral("border-strong")),
        1));
    p.setBrush(
        theme_.color(isChecked() ? QStringLiteral("button-bg") : QStringLiteral("secondary-bg")));
    p.drawRoundedRect(QRectF(2, 4, 38, 18), 9, 9);
    p.setPen(Qt::NoPen);
    p.setBrush(theme_.color(isChecked() ? QStringLiteral("on-accent") : QStringLiteral("muted")));
    p.drawEllipse(QRectF(isChecked() ? 25 : 6, 7, 12, 12));
    if (hasFocus()) {
        p.setPen(QPen(theme_.color(QStringLiteral("focus")), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(0.5, 1.5, 41, 23), 11, 11);
    }
}
ThemePreview::ThemePreview(Theme& theme, QString mode, QWidget* parent)
    : QWidget(parent), theme_(theme), mode_(std::move(mode)) {
    setFixedHeight(108);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void ThemePreview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto draw = [&](const QString& mode) {
        const auto c = [&](const char* role) {
            return theme_.colorForTheme(mode, QString::fromLatin1(role));
        };
        const QRectF frame = QRectF(rect()).adjusted(5, 6, -5, -3);
        p.setPen(QPen(c("border"), 1));
        p.setBrush(c("chrome"));
        p.drawRoundedRect(frame, 4, 4);
        p.fillRect(frame.adjusted(1, 13, -1, -1), c("surface"));
        const qreal nav = frame.width() * .27;
        p.fillRect(QRectF(frame.left() + 1, frame.top() + 13, nav, frame.height() - 14),
                   c("chrome"));
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            p.setBrush(i == 0 ? c("accent") : c("border-strong"));
            p.drawRoundedRect(QRectF(frame.left() + 6, frame.top() + 23 + i * 10, nav - 10, 4), 1,
                              1);
        }
        p.setBrush(c("muted"));
        p.drawRoundedRect(QRectF(frame.left() + nav + 9, frame.top() + 23, frame.width() * .28, 4),
                          1, 1);
        for (int i = 0; i < 3; ++i) {
            const QRectF r(frame.left() + nav + 9, frame.top() + 35 + i * 17,
                           frame.width() - nav - 17, 13);
            p.setPen(QPen(c("border"), 1));
            p.setBrush(c("surface-subtle"));
            p.drawRoundedRect(r, 1, 1);
            p.setPen(Qt::NoPen);
            p.setBrush(c("accent"));
            p.drawRoundedRect(QRectF(r.right() - 18, r.top() + 5, 12, 3), 1.5, 1.5);
        }
    };
    if (mode_ != QStringLiteral("auto"))
        draw(mode_);
    else {
        p.save();
        p.setClipRect(0, 0, width() / 2, height());
        draw(QStringLiteral("light"));
        p.restore();
        p.save();
        p.setClipRect(width() / 2, 0, width(), height());
        draw(QStringLiteral("dark"));
        p.restore();
    }
}
GridPreview::GridPreview(Theme& theme, QWidget* parent) : QWidget(parent), theme_(theme) {
    setMinimumSize(180, 220);
    setAccessibleName(QStringLiteral("启动面板布局预览"));
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void GridPreview::setGrid(int columns, int rows) {
    columns_ = columns;
    rows_ = rows;
    setAccessibleDescription(
        QStringLiteral("%1 列 × %2 行，共 %3 个槽位").arg(columns).arg(rows).arg(columns * rows));
    update();
}
void GridPreview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double unit = qMin((width() - 38.0) / columns_, (height() - 58.0) / rows_);
    const QSizeF grid(unit * columns_, unit * rows_);
    const QRectF frame((width() - grid.width()) / 2 - 9, (height() - grid.height()) / 2 - 18,
                       grid.width() + 18, grid.height() + 36);
    p.setPen(QPen(theme_.color(QStringLiteral("border-strong")), 1));
    p.setBrush(theme_.color(QStringLiteral("surface")));
    p.drawRoundedRect(frame, 6, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(theme_.color(QStringLiteral("accent")));
    p.drawRoundedRect(QRectF(frame.center().x() - 14, frame.bottom() - 9, 28, 3), 1.5, 1.5);
    for (int y = 0; y < rows_; ++y)
        for (int x = 0; x < columns_; ++x) {
            p.setBrush(theme_.color(y * columns_ + x < 6 ? QStringLiteral("accent-soft")
                                                         : QStringLiteral("surface-hover")));
            p.drawRoundedRect(QRectF(frame.left() + 9 + x * unit + 2,
                                     frame.top() + 16 + y * unit + 2, unit - 4, unit - 4),
                              qMin(4.0, unit / 5), qMin(4.0, unit / 5));
        }
}
QLabel* label(const QString& text, const char* role) {
    auto* w = new QLabel(text);
    w->setTextFormat(Qt::PlainText);
    w->setWordWrap(true);
    if (role)
        w->setObjectName(QString::fromLatin1(role));
    return w;
}
QVBoxLayout* content(QScrollArea*& scroll, QWidget* parent) {
    scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("SettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("SettingsContent"));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(28, 20, 28, 28);
    layout->setSpacing(14);
    layout->setAlignment(Qt::AlignTop);
    scroll->setWidget(body);
    return layout;
}
QVBoxLayout* card(QVBoxLayout* layout, const QString& title) {
    if (!title.isEmpty()) {
        if (layout->count())
            layout->addSpacing(6);
        layout->addWidget(label(title, "SettingsSection"));
    }
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("SettingsCard"));
    auto* box = new QVBoxLayout(frame);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    layout->addWidget(frame);
    return box;
}
QVBoxLayout* fields(QVBoxLayout* box) {
    auto* group = new QWidget;
    group->setObjectName(QStringLiteral("SettingsFields"));
    group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(14);
    box->addWidget(group);
    return layout;
}
QPlainTextEdit* textArea(QVBoxLayout* group, const QString& title, const QString& value,
                         const QString& placeholder, const QString& objectName) {
    auto* field = new QVBoxLayout;
    field->setContentsMargins(0, 0, 0, 0);
    field->setSpacing(7);
    auto* heading = label(title, "SettingsFieldLabel");
    auto* edit = new QPlainTextEdit(value);
    edit->setObjectName(objectName);
    edit->setProperty("settingsField", true);
    edit->setAccessibleName(title);
    edit->setPlaceholderText(placeholder);
    edit->setFixedHeight(80);
    // The stylesheet owns text padding; QTextDocument's extra 4px would stack.
    edit->document()->setDocumentMargin(0);
    heading->setBuddy(edit);
    field->addWidget(heading);
    field->addWidget(edit);
    group->addLayout(field);
    return edit;
}
void row(QVBoxLayout* box, const QString& title, const QString& description, QWidget* control) {
    if (box->count()) {
        auto* line = new QFrame;
        line->setObjectName(QStringLiteral("SettingsRule"));
        line->setFixedHeight(1);
        box->addWidget(line);
    }
    auto* w = new QWidget;
    auto* line = new QHBoxLayout(w);
    line->setContentsMargins(16, 14, 16, 14);
    line->setSpacing(20);
    auto* copy = new QVBoxLayout;
    copy->setSpacing(5);
    auto* heading = label(title, "SettingsLabel");
    if (control) {
        heading->setBuddy(control);
        control->setAccessibleName(title);
    }
    copy->addWidget(heading);
    if (!description.isEmpty())
        copy->addWidget(label(description, "SettingsDescription"));
    line->addLayout(copy, 1);
    if (control)
        line->addWidget(control, 0, Qt::AlignVCenter);
    box->addWidget(w);
}
void note(QVBoxLayout* layout, const QString& text) {
    auto* l = label(text, "SettingsNote");
    l->setContentsMargins(16, 11, 16, 11);
    layout->addWidget(l);
}
void iconButton(QPushButton* button, Theme& theme, Symbol symbol) {
    auto update = [button, &theme, symbol] {
        auto icon = theme.icon(symbol, button->isChecked());
        if (button->property("primary").toBool()) {
            QIcon white;
            for (qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0})
                white.addPixmap(icon.pixmap(QSize(16, 16), scale, QIcon::Normal, QIcon::On));
            icon = white;
        }
        button->setIcon(icon);
    };
    update();
    button->setIconSize({16, 16});
    QObject::connect(&theme, &Theme::changed, button, update);
    QObject::connect(button, &QPushButton::toggled, button, update);
}
QStringList processes(const QString& text) {
    QStringList result;
    for (const auto& value :
         text.split(QRegularExpression(QStringLiteral("[;,；，\\r\\n]+")), Qt::SkipEmptyParts))
        if (!value.trimmed().isEmpty())
            result.append(value.trimmed());
    return result;
}
QMessageBox* confirm(QWidget* parent, const QString& title, const QString& text,
                     const QString& acceptLabel, std::function<void()> accepted,
                     const QString& objectName) {
    auto* dialog = new QMessageBox(QMessageBox::Question, title, text,
                                   QMessageBox::Yes | QMessageBox::No, parent);
    dialog->setObjectName(objectName);
    dialog->button(QMessageBox::Yes)->setText(acceptLabel);
    dialog->button(QMessageBox::No)->setText(QStringLiteral("取消"));
    dialog->setDefaultButton(QMessageBox::No);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::finished, parent,
                     [accepted = std::move(accepted)](int result) {
                         if (result == QMessageBox::Yes)
                             accepted();
                     });
    dialog->open();
    return dialog;
}
QString styleSheet() {
    return QStringLiteral(R"(
QDialog#SettingsDialog, QWidget#SettingsContent, QWidget#SettingsHeading, QScrollArea#SettingsScroll { background: @chrome; }
QWidget#SettingsSidebar { background: @chrome; border-right: 1px solid @border; }
QLabel#SettingsBrand { font-size: 22px; font-weight: 700; }
QLabel#SettingsPageTitle { font-size: 24px; font-weight: 600; }
QLabel#SettingsDescription, QLabel#SettingsNavGroup, QLabel#SettingsNote { font-size: 12px; color: @muted; }
QLabel#SettingsSection { font-size: 13px; font-weight: 600; padding-top: 3px; }
QLabel#SettingsFieldLabel { font-size: 12px; }
QComboBox[settingsControl="true"], QSpinBox[settingsControl="true"] { background: @field; border: 1px solid @border-strong; border-radius: 4px; padding: 0px 32px 0px 10px; font-size: 12px; }
/* Qt reserves the spin button width separately from the content padding. */
QSpinBox[settingsControl="true"] { padding-right: 6px; }
QComboBox[settingsControl="true"]:hover, QSpinBox[settingsControl="true"]:hover { border-color: @muted; }
QComboBox[settingsControl="true"]:focus, QSpinBox[settingsControl="true"]:focus { border-color: @focus; }
QComboBox[settingsControl="true"]:disabled, QSpinBox[settingsControl="true"]:disabled { color: @muted; background: @surface-subtle; border-color: @border; }
QSpinBox[settingsControl="true"] QLineEdit { border: none; border-radius: 0px; background: transparent; padding: 0px; }
QComboBox[settingsControl="true"]::drop-down { subcontrol-origin: border; subcontrol-position: top right; width: 30px; border: none; background: transparent; border-top-right-radius: 4px; border-bottom-right-radius: 4px; }
QComboBox[settingsControl="true"]::down-arrow { image: none; width: 0px; height: 0px; }
QSpinBox[settingsControl="true"]::up-button { subcontrol-origin: border; subcontrol-position: top right; top: 1px; right: 1px; width: 26px; height: 16px; border: none; background: transparent; border-top-right-radius: 3px; }
QSpinBox[settingsControl="true"]::down-button { subcontrol-origin: border; subcontrol-position: bottom right; bottom: 1px; right: 1px; width: 26px; height: 16px; border: none; background: transparent; border-bottom-right-radius: 3px; }
QSpinBox[settingsControl="true"]::up-button:hover, QSpinBox[settingsControl="true"]::down-button:hover { background: @surface-hover; }
QSpinBox[settingsControl="true"]::up-button:pressed, QSpinBox[settingsControl="true"]::down-button:pressed { background: @surface-selected; }
QSpinBox[settingsControl="true"]::up-button:off, QSpinBox[settingsControl="true"]::down-button:off { background: transparent; }
QSpinBox[settingsControl="true"]::up-arrow, QSpinBox[settingsControl="true"]::down-arrow { image: none; width: 0px; height: 0px; }
QPlainTextEdit[settingsField="true"] { background: @field; border: 1px solid @border-strong; border-radius: 4px; padding: 7px 10px; font-size: 12px; }
QPlainTextEdit[settingsField="true"]:focus { border-color: @focus; }
QFrame#SettingsCard { background: @surface-subtle; border: 1px solid @border; border-radius: 7px; }
QFrame#SettingsRule { background: @border; border: none; }
QWidget#SettingsFooter { background: @chrome; border-top: 1px solid @border; }
QPushButton#SettingsNav { text-align: left; padding: 11px 12px; border: 1px solid transparent; background: transparent; border-radius: 5px; }
QPushButton#SettingsNav:hover { background: @surface-hover; }
QPushButton#SettingsNav:checked { background: @surface-selected; color: @accent; border-left: 3px solid @accent; padding-left: 10px; }
QPushButton#SettingsNav:focus { border-color: @focus; }
QPushButton#ThemeChoice { background: @surface-subtle; border: 1px solid @border; border-radius: 6px; padding: 5px; }
QPushButton#ThemeChoice:checked { border: 2px solid @accent; padding: 4px; }
QPushButton#ThemeChoice:focus { border-color: @focus; }
QCheckBox[lockChoice="true"] { background: @surface-subtle; border: 1px solid @border; border-radius: 6px; padding: 18px; }
QCheckBox[lockChoice="true"]:checked { border-color: @accent; }
QPushButton[primary="true"] { background: @button-bg; color: @on-accent; border-color: @button-bg; }
QPushButton[primary="true"]:hover { background: @button-hover; }
QPushButton:disabled { color: @muted; }
QPushButton[primary="true"]:disabled { background: @secondary-bg; border-color: @border; color: @muted; }
QLabel#SettingsError { color: @danger; font-size: 12px; }
QPushButton[danger="true"] { color: @danger; }
QTabWidget[settingsTabs="true"]::pane { border: none; background: @chrome; }
QTabWidget[settingsTabs="true"] QTabBar::tab { padding: 10px 16px; border-bottom: 2px solid transparent; }
QTabWidget[settingsTabs="true"] QTabBar::tab:selected { border-bottom-color: @accent; color: @accent; }
QPushButton:checked { background: @surface-selected; border-color: @accent; }
QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: @border-strong; }
QSlider::sub-page:horizontal { background: @button-bg; border-radius: 2px; }
QSlider::handle:horizontal { background: @button-bg; width: 14px; height: 14px; border-radius: 7px; margin: -5px 0; }
QSlider::handle:horizontal:focus { border: 1px solid @focus; }
QTableWidget#gestureRules { background: @surface-subtle; border: 1px solid @border; border-radius: 6px; gridline-color: @border; }
QHeaderView::section { background: @chrome; color: @muted; border: none; padding: 10px; text-align: left; }
QTableWidget::item { padding: 6px; }
)");
}
} // namespace mt::settingsui
