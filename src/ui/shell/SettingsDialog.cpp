#include "SettingsDialog.h"
#include "GestureSettings.h"
#include "SettingsControls.h"
#include "infrastructure/storage/HLaunchImport.h"
#include "infrastructure/storage/HopyImport.h"
#include "modules/screen_lock/UnlockPolicy.h"
#include "platform/windows/LockPassword.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTabWidget>
#include <QTextBrowser>

namespace mt {
using namespace settingsui;
struct SettingsDialog::Impl {
    SettingsDialog* q;
    Theme& theme;
    Settings saved, draft;
    Apply apply;
    QString root;
    bool saving = false, prompting = false, closing = false;
    QStackedWidget* pages = nullptr;
    QLabel *status = nullptr, *error = nullptr;
    QDialogButtonBox* buttons = nullptr;
    QLineEdit* password = nullptr;
    GestureSettings* gestures = nullptr;
    QLabel* updateStatus = nullptr;
    QPushButton *checkUpdate = nullptr, *installUpdate = nullptr, *cancelUpdate = nullptr;
    Impl(SettingsDialog* owner, Theme& t, const Settings& initial, Apply a, QString path)
        : q(owner), theme(t), saved(initial), draft(initial), apply(std::move(a)),
          root(std::move(path)) {}
    bool dirty() const {
        return draft.encode() != saved.encode() || (password && !password->text().isEmpty());
    }
    void changed() {
        if (!status)
            return;
        status->setText(dirty() ? QStringLiteral("有未保存的更改")
                                : QStringLiteral("所有更改已保存"));
        buttons->button(QDialogButtonBox::Save)->setEnabled(dirty() && !saving);
        error->clear();
        error->hide();
    }
    void fail(const QString& text) {
        error->setText(text);
        error->show();
        status->setText(QStringLiteral("保存未完成"));
    }
    void previewTheme() {
        const auto resolved = draft.theme == QStringLiteral("auto")
                                  ? (qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light
                                         ? QStringLiteral("light")
                                         : QStringLiteral("dark"))
                                  : draft.theme;
        theme.apply(resolved);
    }
    Switch* toggle(bool Settings::* field, const char* name, const QString& title) {
        auto* w = new Switch(theme, title);
        w->setObjectName(QString::fromLatin1(name));
        w->setChecked(draft.*field);
        QObject::connect(w, &QCheckBox::toggled, q, [this, field](bool value) {
            draft.*field = value;
            changed();
        });
        return w;
    }
    QSpinBox* number(int Settings::* field, const char* name, int min, int max,
                     const QString& suffix = {}) {
        auto* w = new SpinBox(theme);
        w->setObjectName(QString::fromLatin1(name));
        w->setRange(min, max);
        w->setSuffix(suffix);
        w->setValue(draft.*field);
        w->setFixedWidth(120);
        QObject::connect(w, &QSpinBox::valueChanged, q, [this, field](int v) {
            draft.*field = v;
            changed();
        });
        return w;
    }
    QComboBox* combo(QString Settings::* field, const char* name,
                     std::initializer_list<std::pair<QString, QString>> options) {
        auto* w = new ComboBox(theme);
        w->setObjectName(QString::fromLatin1(name));
        w->setMinimumWidth(166);
        for (const auto& [value, text] : options)
            w->addItem(text, value);
        w->setCurrentIndex(qMax(0, w->findData(draft.*field)));
        QObject::connect(w, &QComboBox::currentIndexChanged, q, [this, field, w] {
            draft.*field = w->currentData().toString();
            changed();
        });
        return w;
    }
    QKeySequenceEdit* shortcut(QString Settings::* field, const char* name) {
        auto* w = new QKeySequenceEdit(
            QKeySequence::fromString(draft.*field, QKeySequence::PortableText));
        w->setObjectName(QString::fromLatin1(name));
        w->setMaximumSequenceLength(1);
        w->setFixedWidth(166);
        w->setToolTip(QStringLiteral("点击后按下新的快捷键"));
        if (auto* edit = w->findChild<QLineEdit*>())
            edit->setPlaceholderText(QStringLiteral("按下快捷键"));
        QObject::connect(w, &QKeySequenceEdit::keySequenceChanged, q, [this, w, field] {
            draft.*field = w->keySequence().toString(QKeySequence::PortableText);
            changed();
        });
        return w;
    }
    QPlainTextEdit* processList(QVBoxLayout* group, const QString& title,
                                QStringList Settings::* field, const char* name,
                                const QString& placeholder) {
        auto* w = textArea(group, title, (draft.*field).join(QStringLiteral("; ")), placeholder,
                           QString::fromLatin1(name));
        QObject::connect(w, &QPlainTextEdit::textChanged, q, [this, w, field] {
            draft.*field = processes(w->toPlainText());
            changed();
        });
        return w;
    }
    QVBoxLayout* page(const QString& title, const QString& description,
                      bool Settings::* enabled = nullptr, const char* name = "") {
        auto* w = new QWidget;
        auto* layout = new QVBoxLayout(w);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* heading = new QWidget;
        heading->setObjectName(QStringLiteral("SettingsHeading"));
        auto* line = new QHBoxLayout(heading);
        line->setContentsMargins(28, 24, 28, 6);
        auto* text = new QVBoxLayout;
        text->setSpacing(7);
        text->addWidget(label(title, "SettingsPageTitle"));
        text->addWidget(label(description, "SettingsDescription"));
        line->addLayout(text, 1);
        if (enabled) {
            auto* state =
                label(draft.*enabled ? QStringLiteral("已启用") : QStringLiteral("未启用"),
                      "SettingsDescription");
            auto* control = toggle(enabled, name, QStringLiteral("启用") + title);
            QObject::connect(control, &QCheckBox::toggled, state, [state](bool on) {
                state->setText(on ? QStringLiteral("已启用") : QStringLiteral("未启用"));
            });
            line->addWidget(state);
            line->addWidget(control);
        }
        layout->addWidget(heading);
        pages->addWidget(w);
        return layout;
    }
    QVBoxLayout* scroll(QVBoxLayout* outer) {
        QScrollArea* area = nullptr;
        auto* layout = content(area, outer->parentWidget());
        outer->addWidget(area, 1);
        return layout;
    }
    QTabWidget* tabs(QVBoxLayout* outer, const char* name) {
        auto* w = new QTabWidget;
        w->setObjectName(QString::fromLatin1(name));
        w->setProperty("settingsTabs", true);
        w->setDocumentMode(true);
        outer->addWidget(w, 1);
        return w;
    }
    QVBoxLayout* tab(QTabWidget* tabs, const QString& text) {
        QScrollArea* area = nullptr;
        auto* layout = content(area, tabs);
        tabs->addTab(area, text);
        return layout;
    }
    void importFolder(bool launcher) {
        auto* chooser = new QFileDialog(q,
                                        launcher ? QStringLiteral("选择 HLaunch 数据文件夹")
                                                 : QStringLiteral("选择 Hopy 数据文件夹"),
                                        launcher ? defaultHLaunchFolder() : defaultHopyFolder());
        chooser->setFileMode(QFileDialog::Directory);
        chooser->setOption(QFileDialog::ShowDirsOnly);
        chooser->setOption(QFileDialog::DontUseNativeDialog);
        chooser->setLabelText(QFileDialog::Accept, QStringLiteral("选择文件夹"));
        chooser->setLabelText(QFileDialog::Reject, QStringLiteral("取消"));
        chooser->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(
            chooser, &QFileDialog::fileSelected, q, [this, launcher](const QString& folder) {
                confirm(q, QStringLiteral("确认导入"),
                        QStringLiteral(
                            "只读导入 %1 "
                            "中的数据并合并到当前数据。原始文件不会被修改。\n导入会立即执行。")
                            .arg(QDir::toNativeSeparators(folder)),
                        QStringLiteral("导入"), [this, launcher, folder] {
                            if (launcher)
                                Q_EMIT q->importHLaunchRequested(folder);
                            else
                                Q_EMIT q->importHopyRequested(folder);
                        });
            });
        chooser->open();
    }
    void buildGeneral();
    void buildAppearance();
    void buildLauncher();
    void buildClipboard();
    void buildGestures();
    void buildLock();
    void buildAbout();
};
void SettingsDialog::Impl::buildGeneral() {
    auto* layout = scroll(page(QStringLiteral("常规"), QStringLiteral("启动方式与数据位置。")));
    auto* box = card(layout, QStringLiteral("启动与运行"));
    auto* startup = new Switch(theme, QStringLiteral("开机启动"));
    startup->setEnabled(false);
    startup->setToolTip(QStringLiteral("开发阶段暂不接管开机启动"));
    row(box, QStringLiteral("开机启动"), QStringLiteral("开发阶段暂不接管开机启动。"), startup);
    row(box, QStringLiteral("启动时隐藏面板"), QStringLiteral("在系统托盘中运行，使用快捷键唤起。"),
        toggle(&Settings::startHidden, "startHidden", QStringLiteral("启动时隐藏面板")));
    box = card(layout, QStringLiteral("数据位置"));
    row(box, QStringLiteral("设置与功能数据"), QStringLiteral("当前实例使用以下目录。"), nullptr);
    auto* path = new QLineEdit(QDir::toNativeSeparators(root));
    path->setReadOnly(true);
    path->setObjectName(QStringLiteral("settingsDataRoot"));
    path->setAccessibleName(QStringLiteral("数据目录"));
    auto* padding = new QHBoxLayout;
    padding->setContentsMargins(16, 0, 16, 8);
    padding->addWidget(path);
    box->addLayout(padding);
    note(box, QStringLiteral("旧工具的数据导入位于对应功能页。"));
    note(layout, QStringLiteral("快捷键可在对应功能中修改；更改后统一保存。"));
}
void SettingsDialog::Impl::buildAppearance() {
    auto* layout =
        scroll(page(QStringLiteral("外观"), QStringLiteral("让 MT 更符合你的使用习惯。")));
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("主题"), "SettingsSection"));
    heading->addStretch();
    heading->addWidget(label(QStringLiteral("切换后即时预览"), "SettingsDescription"));
    layout->addLayout(heading);
    auto* choices = new QHBoxLayout;
    choices->setSpacing(12);
    auto* group = new QButtonGroup(q);
    const QList<std::pair<QString, QString>> modes{
        {QStringLiteral("light"), QStringLiteral("浅色")},
        {QStringLiteral("dark"), QStringLiteral("深色")},
        {QStringLiteral("auto"), QStringLiteral("跟随系统")}};
    for (const auto& [mode, text] : modes) {
        auto* button = new QPushButton;
        button->setObjectName(QStringLiteral("ThemeChoice"));
        button->setProperty("themeValue", mode);
        button->setAccessibleName(text);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setChecked(draft.theme == mode);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFixedHeight(156);
        auto* inner = new QVBoxLayout(button);
        inner->setContentsMargins(6, 3, 6, 11);
        inner->setSpacing(8);
        inner->addWidget(new ThemePreview(theme, mode));
        auto* name = label(text);
        name->setAlignment(Qt::AlignCenter);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        inner->addWidget(name);
        choices->addWidget(button, 1);
        group->addButton(button);
        QObject::connect(button, &QPushButton::clicked, q, [this, mode] {
            draft.theme = mode;
            previewTheme();
            changed();
        });
    }
    layout->addLayout(choices);
    auto* box = card(layout, QStringLiteral("面板不透明度"));
    const auto opacity = [this](int Settings::* field, const char* name, int min) {
        auto* w = new QWidget;
        auto* line = new QHBoxLayout(w);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(12);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(min, 100);
        slider->setValue(draft.*field);
        slider->setFixedWidth(120);
        slider->setAccessibleName(QString::fromLatin1(name) == QStringLiteral("launcherOpacity")
                                      ? QStringLiteral("启动面板不透明度")
                                      : QStringLiteral("剪贴板不透明度"));
        auto* value = number(field, name, min, 100, QStringLiteral("%"));
        value->setFixedWidth(84);
        QObject::connect(slider, &QSlider::valueChanged, value, &QSpinBox::setValue);
        QObject::connect(value, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        QObject::connect(value, &QSpinBox::valueChanged, q, [this] {
            Q_EMIT q->panelOpacityPreview(draft.launcherOpacity, draft.clipboardOpacity);
        });
        line->addWidget(slider);
        line->addWidget(value);
        return w;
    };
    row(box, QStringLiteral("启动面板"), QStringLiteral("面板及附属搜索框使用相同不透明度。"),
        opacity(&Settings::launcherOpacity, "launcherOpacity", 30));
    row(box, QStringLiteral("剪贴板"), QStringLiteral("调低后可以看到面板后方的内容。"),
        opacity(&Settings::clipboardOpacity, "clipboardOpacity", 60));
    note(box, QStringLiteral("100% 为完全不透明。设置窗口始终保持不透明。"));
    box = card(layout, QStringLiteral("字体与图标"));
    row(box, QStringLiteral("清晰、熟悉、恰到好处。"),
        QStringLiteral("跟随 Windows 系统字体 · Lucide 线性图标"), nullptr);
}
void SettingsDialog::Impl::buildLauncher() {
    auto* sections =
        tabs(page(QStringLiteral("启动面板"), QStringLiteral("快捷打开应用、文件和常用位置。"),
                  &Settings::launcherEnabled, "launcherEnabled"),
             "launcherTabs");
    auto* layout = tab(sections, QStringLiteral("常规"));
    auto* box = card(layout, QStringLiteral("唤起与显示"));
    row(box, QStringLiteral("唤起快捷键"), QStringLiteral("显示或隐藏启动面板。"),
        shortcut(&Settings::launcherHotkey, "launcherHotkey"));
    row(box, QStringLiteral("保持面板打开"), QStringLiteral("打开项目或切换到其他窗口后保持显示。"),
        toggle(&Settings::launcherPinned, "launcherPinned", QStringLiteral("保持面板打开")));
    box = card(layout, QStringLiteral("数据导入"));
    auto* import = new QPushButton(QStringLiteral("从 HLaunch 导入…"));
    iconButton(import, theme, Symbol::Upload);
    row(box, QStringLiteral("导入启动项"), QStringLiteral("合并分类与启动项，保留原始数据。"),
        import);
    QObject::connect(import, &QPushButton::clicked, q, [this] { importFolder(true); });
    layout = tab(sections, QStringLiteral("面板布局"));
    box = card(layout, QStringLiteral("网格大小"));
    auto* columns =
        number(&Settings::launcherColumns, "launcherColumns", 3, 20, QStringLiteral(" 列"));
    auto* rows = number(&Settings::launcherRows, "launcherRows", 3, 20, QStringLiteral(" 行"));
    row(box, QStringLiteral("列数"), QStringLiteral("每行显示的槽位数量。"), columns);
    row(box, QStringLiteral("行数"), QStringLiteral("面板高度随行数调整。"), rows);
    auto* grid = new GridPreview(theme);
    grid->setObjectName(QStringLiteral("launcherGridPreview"));
    grid->setGrid(draft.launcherColumns, draft.launcherRows);
    box->addWidget(grid);
    auto* count = label({}, "SettingsDescription");
    count->setAlignment(Qt::AlignCenter);
    box->addWidget(count);
    const auto update = [this, grid, count] {
        grid->setGrid(draft.launcherColumns, draft.launcherRows);
        count->setText(QStringLiteral("%1 列 × %2 行 · %3 个槽位")
                           .arg(draft.launcherColumns)
                           .arg(draft.launcherRows)
                           .arg(draft.launcherColumns * draft.launcherRows));
    };
    QObject::connect(columns, &QSpinBox::valueChanged, q, update);
    QObject::connect(rows, &QSpinBox::valueChanged, q, update);
    update();
    note(box, QStringLiteral("默认 4 列 × 7 行，分类固定在底部。保存后重新排列现有项目。"));
    layout = tab(sections, QStringLiteral("边缘唤起"));
    box = card(layout, QStringLiteral("屏幕边缘"));
    auto* enabled = toggle(&Settings::launcherEdgeEnabled, "launcherEdgeEnabled",
                           QStringLiteral("启用边缘唤起"));
    row(box, QStringLiteral("启用边缘唤起"), QStringLiteral("鼠标移到选定边缘或角落时显示面板。"),
        enabled);
    auto* edges = new QWidget;
    auto* edgeLayout = new QGridLayout(edges);
    edgeLayout->setContentsMargins(16, 12, 16, 12);
    edgeLayout->setSpacing(8);
    const QStringList values{QStringLiteral("topLeft"),
                             QStringLiteral("top"),
                             QStringLiteral("topRight"),
                             QStringLiteral("left"),
                             {},
                             QStringLiteral("right"),
                             QStringLiteral("bottomLeft"),
                             QStringLiteral("bottom"),
                             QStringLiteral("bottomRight")};
    const QStringList names{
        QStringLiteral("左上角"), QStringLiteral("上边缘"), QStringLiteral("右上角"),
        QStringLiteral("左边缘"), QStringLiteral("屏幕"),   QStringLiteral("右边缘"),
        QStringLiteral("左下角"), QStringLiteral("下边缘"), QStringLiteral("右下角")};
    for (int i = 0; i < 9; ++i) {
        if (i == 4) {
            auto* center = label(names[i], "SettingsDescription");
            center->setAlignment(Qt::AlignCenter);
            edgeLayout->addWidget(center, 1, 1);
            continue;
        }
        auto* b = new QPushButton(names[i]);
        b->setCheckable(true);
        // Selection is the checked state; focus must not turn this into a default action.
        b->setAutoDefault(false);
        b->setObjectName(QStringLiteral("edge_") + values[i]);
        b->setChecked(draft.launcherEdgeZones.contains(values[i]));
        b->setMinimumHeight(32);
        edgeLayout->addWidget(b, i / 3, i % 3);
        QObject::connect(b, &QPushButton::toggled, q, [this, key = values[i]](bool on) {
            if (on)
                draft.launcherEdgeZones.append(key);
            else
                draft.launcherEdgeZones.removeAll(key);
            changed();
        });
    }
    box->addWidget(edges);
    edges->setEnabled(enabled->isChecked());
    QObject::connect(enabled, &QCheckBox::toggled, edges, &QWidget::setEnabled);
    row(box, QStringLiteral("多显示器"), QStringLiteral("选择屏幕交界处是否也可触发。"),
        combo(&Settings::launcherEdgeMode, "launcherEdgeMode",
              {{QStringLiteral("outer"), QStringLiteral("仅桌面最外缘")},
               {QStringLiteral("every"), QStringLiteral("每个显示器边缘")}}));
    row(box, QStringLiteral("全屏时暂停唤起"), QStringLiteral("减少游戏、视频播放时的误触。"),
        toggle(&Settings::launcherEdgeSuppressFullscreen, "launcherEdgeSuppressFullscreen",
               QStringLiteral("全屏时暂停唤起")));
    box = card(layout, QStringLiteral("程序规则"));
    auto* lists = fields(box);
    processList(lists, QStringLiteral("禁用程序"), &Settings::launcherEdgeBlocklist,
                "launcherEdgeBlocklist", QStringLiteral("如 game.exe，每行一个程序名"));
    processList(lists, QStringLiteral("仅允许这些程序"), &Settings::launcherEdgeAllowlist,
                "launcherEdgeAllowlist", QStringLiteral("留空表示不限；每行一个程序名"));
    note(box, QStringLiteral("允许名单非空时只在列表内的程序中唤起；同名程序以禁用规则优先。"));
}
void SettingsDialog::Impl::buildClipboard() {
    auto* sections =
        tabs(page(QStringLiteral("剪贴板"), QStringLiteral("找回复制过的内容，快速粘贴。"),
                  &Settings::clipboardEnabled, "clipboardEnabled"),
             "clipboardTabs");
    auto* layout = tab(sections, QStringLiteral("常规"));
    auto* box = card(layout, QStringLiteral("唤起与粘贴"));
    row(box, QStringLiteral("唤起快捷键"), QStringLiteral("显示或隐藏剪贴板面板。"),
        shortcut(&Settings::clipboardHotkey, "clipboardHotkey"));
    row(box, QStringLiteral("确认后自动粘贴"), QStringLiteral("关闭后，确认条目仅复制到剪贴板。"),
        toggle(&Settings::pasteOnConfirm, "pasteOnConfirm", QStringLiteral("确认后自动粘贴")));
    row(box, QStringLiteral("全屏时暂停唤起"), QStringLiteral("避免打断正在使用的全屏程序。"),
        toggle(&Settings::clipboardSuppressFullscreen, "clipboardSuppressFullscreen",
               QStringLiteral("全屏时暂停唤起")));
    row(box, QStringLiteral("面板位置"), QStringLiteral("优先跟随文本光标；未找到时使用此位置。"),
        combo(&Settings::clipboardPlacement, "clipboardPlacement",
              {{QStringLiteral("cursor"), QStringLiteral("鼠标附近")},
               {QStringLiteral("center"), QStringLiteral("屏幕居中")}}));
    box = card(layout, QStringLiteral("记录状态"));
    row(box, QStringLiteral("暂停记录"), QStringLiteral("暂停后保留已有历史，停止收集新内容。"),
        toggle(&Settings::clipboardPaused, "clipboardPaused", QStringLiteral("暂停记录")));
    layout = tab(sections, QStringLiteral("预览与打开"));
    box = card(layout, QStringLiteral("内容预览"));
    row(box, QStringLiteral("悬停预览"), QStringLiteral("鼠标停留在条目上时显示完整内容。"),
        toggle(&Settings::hoverPreview, "hoverPreview", QStringLiteral("悬停预览")));
    row(box, QStringLiteral("空格预览"), QStringLiteral("按空格查看当前选中条目的内容。"),
        toggle(&Settings::spacePreview, "spacePreview", QStringLiteral("空格预览")));
    row(box, QStringLiteral("预览位置"), QStringLiteral("空间不足时自动调整。"),
        combo(&Settings::previewSide, "previewSide",
              {{QStringLiteral("left"), QStringLiteral("面板左侧")},
               {QStringLiteral("right"), QStringLiteral("面板右侧")}}));
    box = card(layout, QStringLiteral("打开链接与文件"));
    auto* openKey = combo(&Settings::openKey, "openKey", {{QString{}, QStringLiteral("禁用")}});
    {
        const QSignalBlocker blocker(openKey);
        for (char c = 'A'; c <= 'Z'; ++c)
            if (!QStringLiteral("DFTSH").contains(QLatin1Char(c)))
                openKey->addItem(QString(QChar::fromLatin1(c)), QString(QChar::fromLatin1(c)));
        openKey->setCurrentIndex(qMax(0, openKey->findData(draft.openKey)));
    }
    row(box, QStringLiteral("打开按键"), QStringLiteral("D / F / T / S / H 已用于其他操作。"),
        openKey);
    auto* mouse = combo(&Settings::openMouseButton, "openMouseButton",
                        {{QStringLiteral("middle"), QStringLiteral("鼠标中键")},
                         {QStringLiteral("right"), QStringLiteral("鼠标右键")},
                         {QStringLiteral("none"), QStringLiteral("禁用")}});
    row(box, QStringLiteral("鼠标打开方式"), QStringLiteral("直接打开选中条目的链接或文件。"),
        mouse);
    note(box, QStringLiteral("使用右键打开时，可通过条目菜单入口管理记录。"));
    layout = tab(sections, QStringLiteral("历史数据"));
    box = card(layout, QStringLiteral("容量与隐私"));
    auto* limit = number(&Settings::historyLimit, "historyLimit", 1, 100000);
    auto* storage = number(&Settings::storageLimit, "storageLimit", 1, 100000);
    QObject::connect(limit, &QSpinBox::valueChanged, storage, [storage](int value) {
        if (storage->value() < value)
            storage->setValue(value);
    });
    QObject::connect(storage, &QSpinBox::editingFinished, q, [limit, storage] {
        if (storage->value() < limit->value())
            storage->setValue(limit->value());
    });
    row(box, QStringLiteral("显示条数"), QStringLiteral("面板中显示的普通历史记录数量。"), limit);
    row(box, QStringLiteral("存储上限"),
        QStringLiteral("自动清理超出上限的普通记录，收藏和置顶保留。"), storage);
    row(box, QStringLiteral("遮蔽敏感内容"), QStringLiteral("对标记为敏感的条目隐藏内容。"),
        toggle(&Settings::maskSensitive, "maskSensitive", QStringLiteral("遮蔽敏感内容")));
    box = card(layout, QStringLiteral("数据管理"));
    auto* import = new QPushButton(QStringLiteral("从 Hopy 导入…"));
    iconButton(import, theme, Symbol::Upload);
    row(box, QStringLiteral("导入历史记录"), QStringLiteral("只读导入旧工具的数据并合并。"),
        import);
    QObject::connect(import, &QPushButton::clicked, q, [this] { importFolder(false); });
    auto* clear = new QPushButton(QStringLiteral("清空历史…"));
    clear->setObjectName(QStringLiteral("clearClipboardHistory"));
    clear->setProperty("danger", true);
    iconButton(clear, theme, Symbol::Delete);
    row(box, QStringLiteral("清空所有记录"), QStringLiteral("包括收藏和置顶，清空后无法恢复。"),
        clear);
    QObject::connect(clear, &QPushButton::clicked, q, [this, clear] {
        clear->setEnabled(false);
        auto* confirmation = confirm(
            q, QStringLiteral("清空历史"),
            QStringLiteral("清空全部剪贴板记录（包括收藏和置顶）？此操作不可撤销。"),
            QStringLiteral("清空全部记录"), [this] { Q_EMIT q->clearHistoryRequested(); },
            QStringLiteral("clearClipboardConfirmation"));
        QObject::connect(confirmation, &QDialog::finished, clear,
                         [clear] { clear->setEnabled(true); });
    });
}
void SettingsDialog::Impl::buildGestures() {
    auto* layout =
        page(QStringLiteral("鼠标手势"), QStringLiteral("按住鼠标键移动，松开执行动作。"),
             &Settings::gesturesEnabled, "gesturesEnabled");
    gestures = new GestureSettings(theme, draft, q);
    layout->addWidget(gestures, 1);
    QObject::connect(gestures, &GestureSettings::changed, q, [this] {
        const auto result = gestures->write(draft);
        changed();
        if (!result)
            fail(result.error());
    });
}
void SettingsDialog::Impl::buildLock() {
    auto* layout =
        scroll(page(QStringLiteral("黑屏遮挡"), QStringLiteral("临时遮挡全部屏幕，并保持唤醒。"),
                    &Settings::lockEnabled, "lockEnabled"));
    auto* box = card(layout, QStringLiteral("启动与暂停"));
    row(box, QStringLiteral("锁定快捷键"), QStringLiteral("开启遮挡；简单模式下也用于解锁。"),
        shortcut(&Settings::lockHotkey, "lockHotkey"));
    row(box, QStringLiteral("暂停黑屏功能"), QStringLiteral("暂时停用快捷键与托盘入口。"),
        toggle(&Settings::lockPaused, "lockPaused", QStringLiteral("暂停黑屏功能")));
    layout->addSpacing(6);
    layout->addWidget(label(QStringLiteral("解锁方式"), "SettingsSection"));
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(q);
    auto* simple = new QCheckBox(QStringLiteral("简单解锁\n按 Enter 或再次按快捷键"));
    auto* protectedMode = new QCheckBox(QStringLiteral("密码解锁\n输入密码后按 Enter"));
    simple->setObjectName(QStringLiteral("simpleUnlock"));
    protectedMode->setObjectName(QStringLiteral("requirePassword"));
    for (auto* w : {simple, protectedMode}) {
        w->setProperty("lockChoice", true);
        group->addButton(w);
        modes->addWidget(w, 1);
    }
    simple->setChecked(!draft.requirePassword);
    protectedMode->setChecked(draft.requirePassword);
    layout->addLayout(modes);
    box = card(layout);
    password = new QLineEdit;
    password->setObjectName(QStringLiteral("lockPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(128);
    password->setMinimumWidth(220);
    password->setPlaceholderText(saved.protectedPassword.isEmpty()
                                     ? QStringLiteral("1–128 位英文字母或数字")
                                     : QStringLiteral("已设置密码；留空保留"));
    row(box, QStringLiteral("解锁密码"),
        QStringLiteral("区分大小写，使用当前 Windows 用户保护保存。"), password);
    auto* modeNote = label({}, "lockModeNote");
    modeNote->setContentsMargins(16, 0, 16, 12);
    box->addWidget(modeNote);
    const auto update = [this, protectedMode, modeNote] {
        draft.requirePassword = protectedMode->isChecked();
        password->setEnabled(draft.requirePassword);
        modeNote->setText(
            draft.requirePassword
                ? QStringLiteral("密码模式：输入密码后按回车解锁。再次按锁定快捷键不会解锁。Esc "
                                 "清空输入，Backspace 删除一位。")
                : QStringLiteral(
                      "简单模式：按回车或再次按锁定快捷键解锁。请先松开锁定时按住的按键。"));
        changed();
    };
    QObject::connect(protectedMode, &QCheckBox::toggled, q, update);
    update();
    QObject::connect(password, &QLineEdit::textChanged, q, [this] { changed(); });
    box = card(layout, QStringLiteral("遮挡期间"));
    row(box, QStringLiteral("覆盖全部显示器"), {},
        label(QStringLiteral("始终启用"), "SettingsDescription"));
    row(box, QStringLiteral("保持系统和显示器唤醒"), {},
        label(QStringLiteral("始终启用"), "SettingsDescription"));
    note(layout,
         QStringLiteral(
             "黑屏遮挡不等同于 Windows 账户锁定。应急时可按 Ctrl + Alt + Del；MT 将解除遮挡。"));
}
void SettingsDialog::Impl::buildAbout() {
    auto* layout =
        scroll(page(QStringLiteral("关于 MT"), QStringLiteral("my tool · 让日常操作更顺手")));
    auto* box = card(layout);
    row(box, QStringLiteral("MT · my tool"),
        QStringLiteral("一个轻巧的入口，安放常用的工具。\n启动面板、剪贴板、鼠标手势与黑屏遮挡。"),
        label(QStringLiteral("版本 ") + QCoreApplication::applicationVersion(),
              "SettingsDescription"));
    box = card(layout, QStringLiteral("软件更新"));
    row(box, QStringLiteral("自动检查更新"),
        QStringLiteral("启动后及每 24 小时检查正式版本；下载和重启由你确认。"),
        toggle(&Settings::autoCheckUpdates, "autoCheckUpdates", QStringLiteral("自动检查更新")));
    updateStatus = label(QStringLiteral("尚未检查更新"), "SettingsDescription");
    updateStatus->setObjectName(QStringLiteral("updateStatus"));
    updateStatus->setWordWrap(true);
    row(box, QStringLiteral("GitHub Releases"), {}, updateStatus);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(16, 0, 16, 12);
    checkUpdate = new QPushButton(QStringLiteral("检查更新"));
    checkUpdate->setObjectName(QStringLiteral("checkUpdate"));
    installUpdate = new QPushButton(QStringLiteral("下载更新"));
    installUpdate->setObjectName(QStringLiteral("installUpdate"));
    cancelUpdate = new QPushButton(QStringLiteral("取消下载"));
    cancelUpdate->setObjectName(QStringLiteral("cancelUpdate"));
    for (auto* button : {checkUpdate, installUpdate, cancelUpdate}) {
        button->setAutoDefault(false);
        button->setEnabled(false);
        actions->addWidget(button);
    }
    actions->addStretch();
    box->addLayout(actions);
    QObject::connect(checkUpdate, &QPushButton::clicked, q, &SettingsDialog::checkUpdatesRequested);
    QObject::connect(installUpdate, &QPushButton::clicked, q, &SettingsDialog::updateRequested);
    QObject::connect(cancelUpdate, &QPushButton::clicked, q, &SettingsDialog::cancelUpdateRequested);
    box = card(layout, QStringLiteral("开源许可"));
    auto* license = new QPushButton(QStringLiteral("查看许可"));
    iconButton(license, theme, Symbol::Open);
    row(box, QStringLiteral("MT、Qt 与第三方组件"),
        QStringLiteral("MT · MIT；Qt · LGPL 3；Lucide · ISC。允许修改并重新链接 Qt。"), license);
    QObject::connect(license, &QPushButton::clicked, q, [this] {
        auto* dialog = new QDialog(q);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("MT · 开源许可"));
        dialog->resize(560, 440);
        auto* layout = new QVBoxLayout(dialog);
        auto* text = new QTextBrowser;
        QString licenses;
        for (const auto& path : {QStringLiteral(":/mt/legal/LICENSE"),
                                 QStringLiteral(":/mt/legal/THIRD_PARTY_NOTICES.md"),
                                 QStringLiteral(":/mt/legal/resources/licenses/LGPL-3.0.txt"),
                                 QStringLiteral(":/mt/legal/resources/licenses/GPL-3.0.txt"),
                                 QStringLiteral(":/mt/legal/resources/licenses/WIL.txt"),
                                 QStringLiteral(":/mt/icons/lucide/LICENSE.txt")}) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly))
                licenses += QString::fromUtf8(file.readAll()) + QStringLiteral("\n\n");
        }
        text->setPlainText(licenses);
        layout->addWidget(text);
        auto* close = new QPushButton(QStringLiteral("关闭"));
        layout->addWidget(close, 0, Qt::AlignRight);
        QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
        dialog->open();
    });
    note(layout, QStringLiteral("原生 Qt 设置界面 · 所有设置在保存后统一生效，外观支持即时预览。"));
}
bool SettingsDialog::hasUnsavedChanges() const { return d->dirty() || d->saving; }
void SettingsDialog::showUpdatePage() { d->pages->setCurrentIndex(6); }
void SettingsDialog::setUpdateStatus(const QString& message, bool canCheck, bool canUpdate,
                                     bool ready, bool canCancel) {
    d->updateStatus->setText(message);
    d->checkUpdate->setEnabled(canCheck);
    d->installUpdate->setEnabled(canUpdate);
    d->installUpdate->setText(ready ? QStringLiteral("安装并重启") : QStringLiteral("下载更新"));
    d->cancelUpdate->setEnabled(canCancel);
    d->cancelUpdate->setText(QStringLiteral("取消"));
}
SettingsDialog::SettingsDialog(Theme& theme, const Settings& settings, const QString& root,
                               Apply apply, QWidget* parent)
    : QDialog(parent), d(std::make_unique<Impl>(this, theme, settings, std::move(apply), root)) {
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(QStringLiteral("MT · 设置"));
    auto settingsFont = font();
    settingsFont.setPixelSize(13);
    setFont(settingsFont);
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);
    setMinimumSize(800, 600);
    const auto available = screen()->availableGeometry().size();
    resize(qMin(1000, available.width() - 60), qMin(760, available.height() - 70));
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("SettingsSidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(188);
    auto* nav = new QVBoxLayout(sidebar);
    nav->setContentsMargins(12, 24, 12, 16);
    nav->setSpacing(5);
    auto* brand = label(QStringLiteral("MT"), "SettingsBrand");
    brand->setContentsMargins(12, 0, 0, 12);
    nav->addWidget(brand);
    auto* right = new QWidget;
    auto* main = new QVBoxLayout(right);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    d->pages = new QStackedWidget;
    d->pages->setObjectName(QStringLiteral("settingsPages"));
    main->addWidget(d->pages, 1);
    outer->addWidget(sidebar);
    outer->addWidget(right, 1);
    auto* group = new QButtonGroup(this);
    const QStringList titles{QStringLiteral("常规"),     QStringLiteral("外观"),
                             QStringLiteral("启动面板"), QStringLiteral("剪贴板"),
                             QStringLiteral("鼠标手势"), QStringLiteral("黑屏遮挡"),
                             QStringLiteral("关于 MT")};
    const QList<Symbol> icons{Symbol::Settings, Symbol::Palette,    Symbol::Grid, Symbol::Copy,
                              Symbol::Gesture,  Symbol::MonitorOff, Symbol::Info};
    for (int i = 0; i < titles.size(); ++i) {
        if (i == 0 || i == 2) {
            if (i == 2)
                nav->addSpacing(18);
            auto* heading = label(i == 0 ? QStringLiteral("偏好设置") : QStringLiteral("功能"),
                                  "SettingsNavGroup");
            heading->setContentsMargins(12, 3, 0, 9);
            nav->addWidget(heading);
        }
        if (i == 6)
            nav->addStretch();
        auto* button = new QPushButton(QStringLiteral("  ") + titles[i]);
        button->setObjectName(QStringLiteral("SettingsNav"));
        button->setProperty("pageIndex", i);
        button->setAccessibleName(titles[i]);
        button->setCheckable(true);
        button->setAutoDefault(false);
        iconButton(button, theme, icons[i]);
        nav->addWidget(button);
        group->addButton(button, i);
        connect(button, &QPushButton::clicked, d->pages,
                [this, i] { d->pages->setCurrentIndex(i); });
    }
    d->buildGeneral();
    d->buildAppearance();
    d->buildLauncher();
    d->buildClipboard();
    d->buildGestures();
    d->buildLock();
    d->buildAbout();
    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(28, 12, 28, 12);
    footerLayout->setSpacing(9);
    d->error = label({}, "SettingsError");
    d->error->setAccessibleName(QStringLiteral("设置错误"));
    d->error->hide();
    footerLayout->addWidget(d->error);
    auto* line = new QHBoxLayout;
    d->status = label({}, "SettingsDescription");
    line->addWidget(d->status, 1);
    d->buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* saveButton = d->buttons->button(QDialogButtonBox::Save);
    saveButton->setText(QStringLiteral("保存设置"));
    saveButton->setProperty("primary", true);
    saveButton->setDefault(false);
    saveButton->setAutoDefault(false);
    auto* cancel = d->buttons->button(QDialogButtonBox::Cancel);
    cancel->setText(QStringLiteral("取消"));
    cancel->setAutoDefault(false);
    connect(d->buttons, &QDialogButtonBox::accepted, this, [this] { save(); });
    connect(d->buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::discard);
    line->addWidget(d->buttons);
    footerLayout->addLayout(line);
    main->addWidget(footer);
    group->button(1)->setChecked(true);
    d->pages->setCurrentIndex(1);
    d->changed();
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (d->draft.theme == QStringLiteral("auto"))
            d->previewTheme();
    });
}
SettingsDialog::~SettingsDialog() = default;
void SettingsDialog::save(bool closeAfter) {
    if (d->saving)
        return;
    auto next = d->draft;
    if (auto result = d->gestures->write(next); !result) {
        d->fail(result.error());
        return;
    }
    if (!d->password->text().isEmpty()) {
        auto plain = d->password->text();
        if (!UnlockPolicy::validPassword(plain)) {
            plain.fill(QChar{});
            d->fail(QStringLiteral("密码须为 1–128 位英文字母或数字，区分大小写"));
            d->password->setFocus();
            return;
        }
        auto protectedValue = protectLockPassword(plain);
        plain.fill(QChar{});
        if (!protectedValue) {
            d->fail(protectedValue.error());
            return;
        }
        next.protectedPassword = *protectedValue;
    }
    if (auto result = next.validate(); !result) {
        d->fail(result.error());
        return;
    }
    d->saving = true;
    setEnabled(false);
    d->status->setText(QStringLiteral("正在保存…"));
    const QPointer<SettingsDialog> guard(this);
    d->apply(next, [guard, next, closeAfter](auto result) {
        if (!guard)
            return;
        auto& data = *guard->d;
        data.saving = false;
        guard->setEnabled(true);
        if (!result) {
            data.fail(result.error());
            return;
        }
        data.saved = next;
        data.draft = next;
        {
            const QSignalBlocker blocker(data.password);
            data.password->clear();
        }
        data.password->setPlaceholderText(next.protectedPassword.isEmpty()
                                              ? QStringLiteral("1–128 位英文字母或数字")
                                              : QStringLiteral("已设置密码；留空保留"));
        data.changed();
        if (closeAfter) {
            data.closing = true;
            guard->accept();
        }
    });
}
void SettingsDialog::discard() {
    if (d->saving)
        return;
    d->draft = d->saved;
    d->previewTheme();
    Q_EMIT panelOpacityPreview(d->saved.launcherOpacity, d->saved.clipboardOpacity);
    d->closing = true;
    QDialog::reject();
}
void SettingsDialog::reject() {
    if (d->saving || d->prompting)
        return;
    if (!d->dirty()) {
        discard();
        return;
    }
    d->prompting = true;
    auto* dialog =
        new QMessageBox(QMessageBox::Question, QStringLiteral("保存设置？"),
                        QStringLiteral("还有未保存的更改。"), QMessageBox::NoButton, this);
    dialog->setObjectName(QStringLiteral("unsavedSettingsConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* saveButton = dialog->addButton(QStringLiteral("保存并关闭"), QMessageBox::AcceptRole);
    auto* discardButton =
        dialog->addButton(QStringLiteral("放弃更改"), QMessageBox::DestructiveRole);
    auto* cancel = dialog->addButton(QStringLiteral("继续编辑"), QMessageBox::RejectRole);
    dialog->setDefaultButton(cancel);
    dialog->setEscapeButton(cancel);
    connect(dialog, &QDialog::finished, this, [this, dialog, saveButton, discardButton] {
        d->prompting = false;
        if (dialog->clickedButton() == saveButton)
            save(true);
        else if (dialog->clickedButton() == discardButton)
            discard();
    });
    dialog->open();
}
void SettingsDialog::closeEvent(QCloseEvent* event) {
    if (d->closing)
        event->accept();
    else {
        event->ignore();
        reject();
    }
}
} // namespace mt
