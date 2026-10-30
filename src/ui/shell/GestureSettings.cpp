#include "GestureSettings.h"
#include "SettingsControls.h"
#include "modules/gestures/GestureRules.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

namespace mt {
using namespace settingsui;
struct GestureSettings::Impl {
    struct Rule {
        QString process, direction, action;
    };
    GestureSettings* q;
    Theme& theme;
    QVector<Rule> rules;
    QComboBox *filter = nullptr, *trigger = nullptr;
    QSpinBox *threshold = nullptr, *timeout = nullptr;
    QCheckBox *trail = nullptr, *name = nullptr, *fullscreen = nullptr;
    QPlainTextEdit *excluded = nullptr, *allowed = nullptr;
    QTableWidget* table = nullptr;
    QLabel* count = nullptr;
    Impl(GestureSettings* owner, Theme& t) : q(owner), theme(t) {}
    void refresh() {
        table->setRowCount(0);
        for (int i = 0; i < rules.size(); ++i) {
            const auto& r = rules[i];
            if ((filter->currentIndex() == 1 && !r.process.isEmpty()) ||
                (filter->currentIndex() == 2 && r.process.isEmpty()))
                continue;
            const auto n = table->rowCount();
            table->insertRow(n);
            auto* direction = new QTableWidgetItem(gestureArrows(r.direction));
            auto f = direction->font();
            f.setPixelSize(21);
            direction->setFont(f);
            direction->setTextAlignment(Qt::AlignCenter);
            table->setItem(n, 0, direction);
            const auto action = GestureAction::parse(r.action);
            auto* label = new QTableWidgetItem(action ? action->label() : r.action);
            label->setToolTip(label->text());
            table->setItem(n, 1, label);
            table->setItem(
                n, 2,
                new QTableWidgetItem(r.process.isEmpty() ? QStringLiteral("全局") : r.process));
            auto* buttons = new QWidget;
            auto* line = new QHBoxLayout(buttons);
            line->setContentsMargins(3, 4, 3, 4);
            line->setSpacing(3);
            auto* edit = new QPushButton;
            edit->setObjectName(QStringLiteral("editGesture"));
            edit->setFixedSize(30, 30);
            edit->setAccessibleName(QStringLiteral("编辑手势 ") + direction->text());
            edit->setToolTip(QStringLiteral("编辑"));
            iconButton(edit, theme, Symbol::Pencil);
            auto* remove = new QPushButton;
            remove->setObjectName(QStringLiteral("removeGesture"));
            remove->setFixedSize(30, 30);
            remove->setAccessibleName(QStringLiteral("删除手势 ") + direction->text());
            remove->setToolTip(QStringLiteral("删除"));
            iconButton(remove, theme, Symbol::Delete);
            line->addWidget(edit);
            line->addWidget(remove);
            table->setCellWidget(n, 3, buttons);
            QObject::connect(edit, &QPushButton::clicked, q, [this, i] { editor(i); });
            QObject::connect(remove, &QPushButton::clicked, q, [this, i] {
                confirm(
                    q, QStringLiteral("删除手势"),
                    QStringLiteral("删除这条手势规则？保存设置后生效。"), QStringLiteral("删除"),
                    [this, i] {
                        rules.removeAt(i);
                        refresh();
                        Q_EMIT q->changed();
                    },
                    QStringLiteral("deleteGestureConfirmation"));
            });
        }
        count->setText(QStringLiteral("%1 条规则").arg(table->rowCount()));
    }
    void editor(int index) {
        const auto initial = index < 0 ? Rule{{}, {}, QStringLiteral("cmd:none")} : rules[index];
        auto* dialog = new QDialog(q);
        dialog->setObjectName(QStringLiteral("gestureEditor"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(index < 0 ? QStringLiteral("添加手势") : QStringLiteral("编辑手势"));
        dialog->resize(520, 510);
        auto* outer = new QVBoxLayout(dialog);
        outer->setContentsMargins(24, 20, 24, 20);
        outer->setSpacing(14);
        auto* form = new QFormLayout;
        form->setVerticalSpacing(12);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        outer->addLayout(form);
        auto* process = new QLineEdit(initial.process);
        process->setObjectName(QStringLiteral("gestureProcess"));
        process->setPlaceholderText(QStringLiteral("留空为全局，例如 chrome.exe"));
        form->addRow(QStringLiteral("适用程序"), process);
        auto* direction = new QLineEdit(initial.direction);
        direction->setObjectName(QStringLiteral("gestureDirections"));
        direction->setMaxLength(30);
        direction->setPlaceholderText(QStringLiteral("例如 26 表示 ↓→"));
        form->addRow(QStringLiteral("方向顺序"), direction);
        auto* pad = new QWidget;
        auto* grid = new QGridLayout(pad);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(6);
        const QString order = QStringLiteral("789456123");
        for (int n = 0; n < 9; ++n) {
            const auto key = order[n];
            auto* b =
                new QPushButton(key == u'5' ? QStringLiteral("清空") : gestureArrows(QString(key)));
            b->setAccessibleName(key == u'5' ? QStringLiteral("清空方向")
                                             : QStringLiteral("添加方向 ") + QString(key));
            b->setAutoDefault(false);
            grid->addWidget(b, n / 3, n % 3);
            QObject::connect(b, &QPushButton::clicked, dialog, [direction, key] {
                if (key == u'5')
                    direction->clear();
                else
                    direction->setText(direction->text() + key);
            });
        }
        form->addRow(QStringLiteral("选择方向"), pad);
        auto* action = new ComboBox(theme);
        action->setObjectName(QStringLiteral("gestureAction"));
        for (const auto& command : gestureCommands())
            action->addItem(gestureCommandLabel(command), QStringLiteral("cmd:") + command);
        action->addItem(QStringLiteral("发送快捷键"), QStringLiteral("key:"));
        action->addItem(QStringLiteral("打开程序或文件"), QStringLiteral("run:"));
        const auto parsed = GestureAction::parse(initial.action);
        const auto kind =
            parsed && parsed->kind == GestureAction::Kind::Shortcut ? QStringLiteral("key:")
            : parsed && parsed->kind == GestureAction::Kind::Open   ? QStringLiteral("run:")
                                                                    : initial.action;
        action->setCurrentIndex(qMax(0, action->findData(kind)));
        form->addRow(QStringLiteral("执行动作"), action);
        auto* shortcut = new QKeySequenceEdit;
        shortcut->setMaximumSequenceLength(1);
        shortcut->setObjectName(QStringLiteral("gestureShortcut"));
        if (auto* edit = shortcut->findChild<QLineEdit*>())
            edit->setPlaceholderText(QStringLiteral("按下快捷键"));
        if (kind == QStringLiteral("key:") && parsed)
            shortcut->setKeySequence(
                QKeySequence::fromString(parsed->value, QKeySequence::PortableText));
        form->addRow(QStringLiteral("快捷键"), shortcut);
        auto* target = new QLineEdit;
        target->setObjectName(QStringLiteral("gestureOpenTarget"));
        target->setPlaceholderText(QStringLiteral("程序、文件、文件夹或网址"));
        if (kind == QStringLiteral("run:") && parsed)
            target->setText(parsed->value);
        form->addRow(QStringLiteral("打开目标"), target);
        const auto update = [action, shortcut, target] {
            shortcut->setEnabled(action->currentData() == QStringLiteral("key:"));
            target->setEnabled(action->currentData() == QStringLiteral("run:"));
        };
        QObject::connect(action, &QComboBox::currentIndexChanged, dialog, update);
        update();
        auto* error = label({}, "SettingsError");
        error->setObjectName(QStringLiteral("gestureError"));
        outer->addWidget(error);
        outer->addStretch();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        outer->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(
            buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, index, process, direction, action, shortcut, target, error] {
                const auto directions = normalizeGesture(direction->text());
                if (!directions) {
                    error->setText(directions.error());
                    direction->setFocus();
                    return;
                }
                auto value = action->currentData().toString();
                if (value == QStringLiteral("key:"))
                    value += shortcut->keySequence().toString(QKeySequence::PortableText);
                else if (value == QStringLiteral("run:"))
                    value += target->text().trimmed();
                const auto program = process->text().trimmed().toCaseFolded();
                for (int i = 0; i < rules.size(); ++i)
                    if (i != index && rules[i].direction == *directions &&
                        rules[i].process.toCaseFolded() == program) {
                        error->setText(QStringLiteral("同一程序内的手势方向不能重复"));
                        direction->setFocus();
                        return;
                    }
                const QJsonObject mapping{{*directions, value}};
                const auto checked = GestureRules::parse(
                    program.isEmpty() ? mapping : QJsonObject{},
                    program.isEmpty() ? QJsonObject{} : QJsonObject{{program, mapping}});
                if (!checked) {
                    error->setText(checked.error());
                    return;
                }
                const Rule rule{program, *directions, value};
                if (index < 0)
                    rules.append(rule);
                else
                    rules[index] = rule;
                refresh();
                Q_EMIT q->changed();
                dialog->accept();
            });
        dialog->open();
        direction->setFocus();
    }
};
GestureSettings::GestureSettings(Theme& theme, const Settings& s, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(this, theme)) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* tabs = new QTabWidget;
    tabs->setProperty("settingsTabs", true);
    tabs->setObjectName(QStringLiteral("gestureTabs"));
    tabs->setDocumentMode(true);
    outer->addWidget(tabs);
    auto* rules = new QWidget;
    auto* layout = new QVBoxLayout(rules);
    layout->setContentsMargins(28, 20, 28, 24);
    layout->setSpacing(14);
    auto* toolbar = new QHBoxLayout;
    d->count = label({}, "SettingsSection");
    toolbar->addWidget(d->count);
    toolbar->addStretch();
    d->filter = new ComboBox(theme);
    d->filter->setObjectName(QStringLiteral("gestureFilter"));
    d->filter->addItems(
        {QStringLiteral("全部规则"), QStringLiteral("全局规则"), QStringLiteral("应用专属")});
    toolbar->addWidget(d->filter);
    auto* add = new QPushButton(QStringLiteral("添加手势"));
    add->setObjectName(QStringLiteral("addGesture"));
    add->setProperty("primary", true);
    iconButton(add, theme, Symbol::Plus);
    toolbar->addWidget(add);
    layout->addLayout(toolbar);
    d->table = new QTableWidget(0, 4);
    d->table->setObjectName(QStringLiteral("gestureRules"));
    d->table->setAccessibleName(QStringLiteral("手势规则"));
    d->table->setHorizontalHeaderLabels({QStringLiteral("手势"), QStringLiteral("动作"),
                                         QStringLiteral("适用程序"), QStringLiteral("操作")});
    d->table->verticalHeader()->hide();
    d->table->verticalHeader()->setDefaultSectionSize(49);
    d->table->setShowGrid(false);
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setSelectionMode(QAbstractItemView::SingleSelection);
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    d->table->setColumnWidth(0, 92);
    d->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    d->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    d->table->setColumnWidth(2, 124);
    d->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    d->table->setColumnWidth(3, 76);
    d->table->setMinimumHeight(200);
    layout->addWidget(d->table, 1);
    layout->addWidget(
        label(QStringLiteral("应用专属规则优先于相同方向的全局规则。"), "SettingsDescription"));
    tabs->addTab(rules, QStringLiteral("手势规则"));
    const auto load = [this](const QString& process, const QJsonObject& actions) {
        for (auto i = actions.begin(); i != actions.end(); ++i)
            d->rules.append({process, i.key(), i.value().toString()});
    };
    load({}, s.gestureActions);
    for (auto i = s.gestureApplicationActions.begin(); i != s.gestureApplicationActions.end(); ++i)
        load(i.key(), i.value().toObject());
    connect(d->filter, &QComboBox::currentIndexChanged, this, [this] { d->refresh(); });
    connect(add, &QPushButton::clicked, this, [this] { d->editor(-1); });
    d->refresh();
    const auto tab = [&](const QString& title) {
        QScrollArea* scroll = nullptr;
        auto* body = content(scroll, tabs);
        tabs->addTab(scroll, title);
        return body;
    };
    const auto toggle = [&](bool initial, const QString& title) {
        auto* w = new Switch(theme, title);
        w->setChecked(initial);
        connect(w, &QCheckBox::toggled, this, &GestureSettings::changed);
        return w;
    };
    layout = tab(QStringLiteral("触发与反馈"));
    auto* box = card(layout, QStringLiteral("触发方式"));
    d->trigger = new ComboBox(theme);
    d->trigger->setObjectName(QStringLiteral("gestureTrigger"));
    d->trigger->setFixedWidth(150);
    d->trigger->addItem(QStringLiteral("右键"), QStringLiteral("right"));
    d->trigger->addItem(QStringLiteral("中键"), QStringLiteral("middle"));
    d->trigger->addItem(QStringLiteral("侧键 M4"), QStringLiteral("x1"));
    d->trigger->addItem(QStringLiteral("侧键 M5"), QStringLiteral("x2"));
    d->trigger->setCurrentIndex(d->trigger->findData(s.gestureTrigger));
    connect(d->trigger, &QComboBox::currentIndexChanged, this, &GestureSettings::changed);
    row(box, QStringLiteral("触发键"), QStringLiteral("普通点击保留，按住并移动开始手势。"),
        d->trigger);
    d->threshold = new SpinBox(theme);
    d->threshold->setObjectName(QStringLiteral("gestureThreshold"));
    d->threshold->setRange(5, 200);
    d->threshold->setValue(s.gestureThreshold);
    d->threshold->setSuffix(QStringLiteral(" 像素"));
    d->threshold->setFixedWidth(150);
    row(box, QStringLiteral("方向分段阈值"), QStringLiteral("移动超过此距离后记录一个方向。"),
        d->threshold);
    connect(d->threshold, &QSpinBox::valueChanged, this, &GestureSettings::changed);
    d->timeout = new SpinBox(theme);
    d->timeout->setObjectName(QStringLiteral("gestureTimeout"));
    d->timeout->setRange(0, 10000);
    d->timeout->setSingleStep(200);
    d->timeout->setSpecialValueText(QStringLiteral("不超时"));
    d->timeout->setValue(s.gestureTimeout);
    d->timeout->setSuffix(QStringLiteral(" 毫秒"));
    d->timeout->setFixedWidth(150);
    row(box, QStringLiteral("停顿取消"), QStringLiteral("停止移动达到此时长后取消手势。"),
        d->timeout);
    connect(d->timeout, &QSpinBox::valueChanged, this, &GestureSettings::changed);
    box = card(layout, QStringLiteral("视觉反馈"));
    d->trail = toggle(s.gestureTrail, QStringLiteral("显示轨迹"));
    d->name = toggle(s.gestureShowName, QStringLiteral("显示动作名称"));
    row(box, QStringLiteral("显示轨迹"), {}, d->trail);
    row(box, QStringLiteral("显示动作名称"), QStringLiteral("在屏幕底部显示当前匹配的动作。"),
        d->name);
    note(layout, QStringLiteral("按 Esc 取消，松开触发键执行动作。"));
    layout = tab(QStringLiteral("生效范围"));
    box = card(layout);
    d->fullscreen = toggle(s.disableGesturesFullscreen, QStringLiteral("全屏程序中禁用"));
    row(box, QStringLiteral("全屏程序中禁用"), {},
        d->fullscreen);
    box = card(layout, QStringLiteral("程序规则"));
    auto* lists = fields(box);
    d->excluded = textArea(
        lists, QStringLiteral("禁用程序"), s.excludedProcesses.join(QStringLiteral("; ")),
        QStringLiteral("如 game.exe，每行一个程序名"), QStringLiteral("excludedProcesses"));
    d->allowed = textArea(lists, QStringLiteral("仅允许这些程序"),
                          s.gestureAllowlist.join(QStringLiteral("; ")),
                          QStringLiteral("留空表示不限"), QStringLiteral("gestureAllowlist"));
    note(box, QStringLiteral("只填写程序文件名，禁用名单优先。应用专属规则在“手势规则”中添加。"));
    for (auto* edit : {d->excluded, d->allowed}) {
        connect(edit, &QPlainTextEdit::textChanged, this, &GestureSettings::changed);
    }
}
GestureSettings::~GestureSettings() = default;
std::expected<void, QString> GestureSettings::write(Settings& s) const {
    QJsonObject global, apps;
    for (const auto& r : d->rules) {
        if (r.process.isEmpty())
            global.insert(r.direction, r.action);
        else {
            auto rules = apps.value(r.process).toObject();
            rules.insert(r.direction, r.action);
            apps.insert(r.process, rules);
        }
    }
    s.gestureActions = global;
    s.gestureApplicationActions = apps;
    s.excludedProcesses = processes(d->excluded->toPlainText());
    s.gestureAllowlist = processes(d->allowed->toPlainText());
    s.gestureTrigger = d->trigger->currentData().toString();
    s.gestureThreshold = d->threshold->value();
    s.gestureTimeout = d->timeout->value();
    s.gestureTrail = d->trail->isChecked();
    s.gestureShowName = d->name->isChecked();
    s.disableGesturesFullscreen = d->fullscreen->isChecked();
    auto parsed = GestureRules::parse(global, apps, s.excludedProcesses, s.gestureAllowlist);
    if (!parsed)
        return std::unexpected(parsed.error());
    s.excludedProcesses = parsed->excluded;
    s.gestureAllowlist = parsed->allowed;
    return {};
}
} // namespace mt
