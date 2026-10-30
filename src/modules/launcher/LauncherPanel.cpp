#include "LauncherPanel.h"
#include "infrastructure/i18n/I18n.h"
#include "ui/components/Theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace mt {
namespace {
const QString mimeType = QStringLiteral("application/x-mt-launch-item");
}
LauncherPanel::LauncherPanel(Theme& theme, LauncherService& service)
    : Panel(theme, tx(u"MT · 启动面板")), theme_(theme), service_(service) {
    setAcceptDrops(true);
    auto* title = theme.header(this);
    auto* header = static_cast<QHBoxLayout*>(title->layout());
    auto* menu = theme.button(Symbol::Menu, tx(u"启动器菜单"), title);
    menuButton_ = menu;
    header->addWidget(menu);
    auto* label = new QLabel(title);
    hoverTitle_ = label;
    auto titleFont = label->font();
    titleFont.setPixelSize(13);
    titleFont.setWeight(QFont::DemiBold);
    label->setFont(titleFont);
    label->setObjectName(QStringLiteral("PanelTitle"));
    label->setTextFormat(Qt::PlainText);
    label->setAlignment(Qt::AlignCenter);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(label, 1);
    pin_ = theme.button(Symbol::Pin, tx(u"保持打开（Ctrl+Space）"), title, true);
    header->addWidget(pin_);
    auto* close = theme.button(Symbol::Close, tx(u"隐藏启动面板（Esc）"), title);
    closeButton_ = close;
    header->addWidget(close);
    for (auto* action : {menu, pin_, close})
        action->setObjectName(QStringLiteral("LauncherHeaderAction"));
    layoutColumn()->addWidget(title);
    enableTitleDrag(title);
    enableTitleDrag(label);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    connect(pin_, &QToolButton::clicked, this, [this](bool checked) {
        keepOpen = checked;
        Q_EMIT pinnedChanged(checked);
    });
    connect(menu, &QToolButton::clicked, this, [this, menu] {
        QMenu popup(this);
        popup.addAction(tx(u"添加项目（Insert）"), this, &LauncherPanel::addItem);
        popup.addAction(tx(u"搜索所有分类（Ctrl+F）"), this, [this] { showSearch(); });
        popup.addAction(tx(u"上一页（PageUp）"), this, [this] { changePage(-1); })
            ->setEnabled(page_ > 0);
        popup.addAction(tx(u"下一页（PageDown）"), this, [this] { changePage(1); })
            ->setEnabled(page_ < lastPage());
        popup.addSeparator();
        popup.addAction(tx(u"设置"), this, [this] { Q_EMIT settingsRequested(); });
        popup.exec(menu->mapToGlobal(QPoint(0, menu->height() + 2)));
    });
    gridWidget_ = new QWidget(this);
    grid_ = new QGridLayout(gridWidget_);
    grid_->setContentsMargins(9, 8, 9, 8);
    grid_->setHorizontalSpacing(4);
    grid_->setVerticalSpacing(4);
    gridScroll_ = new QScrollArea(this);
    gridScroll_->setFocusPolicy(Qt::NoFocus);
    gridScroll_->setFrameShape(QFrame::NoFrame);
    gridScroll_->setWidget(gridWidget_);
    gridScroll_->setWidgetResizable(false);
    enableTitleDrag(gridWidget_);
    enableTitleDrag(gridScroll_->viewport());
    layoutColumn()->addWidget(gridScroll_, 1);
    // The bar owns the rounded bottom corners; tabs stay inset with the grid so
    // the selected underline is straight, as in the original HLaunch.
    auto* bottomBar = new QWidget(this);
    bottomBar->setObjectName(QStringLiteral("BottomBar"));
    bottomBar->setAttribute(Qt::WA_StyledBackground);
    enableTitleDrag(bottomBar);
    auto* bottomRow = new QHBoxLayout(bottomBar);
    bottomRow->setContentsMargins(9, 0, 9, 0);
    bottomRow->setSpacing(0);
    tabs_ = new QTabBar(bottomBar);
    tabs_->setObjectName(QStringLiteral("LauncherCategories"));
    tabs_->setAccessibleName(tx(u"启动项分类"));
    tabs_->setFocusPolicy(Qt::NoFocus);
    tabs_->installEventFilter(this);
    auto tabFont = tabs_->font();
    tabFont.setPixelSize(13);
    tabFont.setWeight(QFont::Medium);
    tabs_->setFont(tabFont);
    tabs_->setExpanding(true);
    tabs_->setMovable(true);
    tabs_->setFixedHeight(30);
    tabs_->setDrawBase(false);
    tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Overflowing categories scroll with one chevron at each end of the bar.
    const auto arrow = [&](Symbol symbol, const QString& label) {
        auto* button = theme.button(symbol, label, bottomBar);
        button->setObjectName(QStringLiteral("TabScroll"));
        button->setFixedSize(18, 30);
        button->setIconSize(QSize(14, 14));
        button->setAutoRepeat(true);
        button->hide();
        return button;
    };
    tabLeft_ = arrow(Symbol::ChevronLeft, tx(u"向左滚动分类"));
    tabRight_ = arrow(Symbol::ChevronRight, tx(u"向右滚动分类"));
    for (auto* child : tabs_->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly)) {
        const auto name = child->objectName();
        auto*& slot = name == QStringLiteral("ScrollLeftButton")    ? qtLeft_
                      : name == QStringLiteral("ScrollRightButton") ? qtRight_
                      : !qtLeft_                                    ? qtLeft_
                                                                    : qtRight_;
        slot = child;
        // Zero size: Qt still shows/enables them, which drives updateTabArrows().
        child->setMaximumSize(0, 0);
        child->installEventFilter(this);
    }
    connect(tabLeft_, &QToolButton::clicked, this, [this] {
        if (qtLeft_)
            qtLeft_->click();
    });
    connect(tabRight_, &QToolButton::clicked, this, [this] {
        if (qtRight_)
            qtRight_->click();
    });
    bottomRow->addWidget(tabLeft_);
    bottomRow->addWidget(tabs_);
    bottomRow->addWidget(tabRight_);
    layoutColumn()->addWidget(bottomBar);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0)
            return;
        const auto id = tabs_->tabData(index).toString();
        // Moving the current tab changes its index, not the selected category.
        if (id == categoryId())
            return;
        page_ = 0;
        displayedCategory_ = id;
        selectionPending_ = true;
        const auto generation = ++selectionGeneration_;
        refresh();
        service_.selectCategory(
            displayedCategory_, [guard = QPointer<LauncherPanel>(this), generation](auto) {
                if (!guard || generation != guard->selectionGeneration_)
                    return;
                guard->selectionPending_ = false;
                guard->displayedCategory_ = guard->service_.catalog().selectedCategory;
                guard->refresh();
            });
    });
    connect(tabs_, &QTabBar::tabMoved, this, [this] {
        tabOrderDirty_ = true;
        if (!tabInteraction_)
            QTimer::singleShot(0, this, &LauncherPanel::commitTabOrder);
    });
    connect(tabs_, &QWidget::customContextMenuRequested, this, &LauncherPanel::categoryMenu);
    auto* searchPanel = new Panel(theme, tx(u"MT · 搜索启动项"), this);
    searchPopup_ = searchPanel;
    searchPopup_->installEventFilter(this);
    connect(this, &Panel::opacityChanged, searchPanel,
            [searchPanel](qreal value) { searchPanel->setPanelOpacity(qRound(value * 100)); });
    setGrid(service_.catalog().columns, LauncherCatalog::defaultRows);
    auto* searchLayout = searchPanel->layoutColumn();
    searchLayout->setContentsMargins(8, 8, 8, 8);
    searchLayout->setSpacing(6);
    search_ = new QLineEdit(searchPopup_);
    search_->setPlaceholderText(tx(u"搜索所有分类"));
    search_->setAccessibleName(tx(u"搜索启动项"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    searchLayout->addWidget(search_);
    results_ = new QListWidget(searchPopup_);
    results_->setObjectName(QStringLiteral("LauncherSearchResults"));
    results_->setAccessibleName(tx(u"跨分类搜索结果"));
    results_->setIconSize(QSize(20, 20));
    results_->setSpacing(2);
    results_->setUniformItemSizes(true);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setTextElideMode(Qt::ElideRight);
    results_->setMaximumHeight(210);
    results_->installEventFilter(this);
    searchLayout->addWidget(results_);
    connect(results_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &LauncherPanel::requestSearchIcons);
    searchEmpty_ = new QLabel(tx(u"没有匹配的启动项\n换个关键词试试"), searchPopup_);
    searchEmpty_->setObjectName(QStringLiteral("Muted"));
    searchEmpty_->setAlignment(Qt::AlignCenter);
    searchEmpty_->setWordWrap(true);
    searchEmpty_->setMinimumHeight(64);
    searchEmpty_->hide();
    searchLayout->addWidget(searchEmpty_);
    connect(search_, &QLineEdit::textChanged, this, [this] { updateSearch(); });
    connect(results_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { activateItem(item->data(Qt::UserRole).toString()); });
    connect(&service_, &LauncherService::changed, this, &LauncherPanel::refresh);
    connect(&theme, &Theme::changed, this, [this] {
        applyTileStyle();
        refresh();
    });
    // The host must connect iconRequested before the first request is emitted.
    QTimer::singleShot(0, this, [this] {
        iconsReady_ = true;
        refresh();
    });
}
QString LauncherPanel::categoryId() const {
    return displayedCategory_.isEmpty() ? service_.catalog().selectedCategory : displayedCategory_;
}
QString LauncherPanel::itemAt(int index) const {
    const auto& c = service_.catalog();
    const int category = c.categoryIndex(categoryId());
    if (category < 0)
        return {};
    return c.categories[category].slots.value(absoluteSlot(index));
}
void LauncherPanel::setPinned(bool pinned) {
    keepOpen = pinned;
    pin_->setChecked(pinned);
}
void LauncherPanel::setGrid(int columns, int rows) {
    columns = qBound(LauncherCatalog::minGrid, columns, LauncherCatalog::maxGrid);
    rows = qBound(LauncherCatalog::minGrid, rows, LauncherCatalog::maxGrid);
    if (columns == columns_ && rows == rows_)
        return;
    // Keep the first visible slot on screen when the page size changes.
    const int first = page_ * qMax(1, pageSize());
    hoveredTile_ = -1;
    updateHoverTitle();
    qDeleteAll(tiles_);
    tiles_.clear();
    columns_ = columns;
    rows_ = rows;
    for (int i = 0; i < pageSize(); ++i) {
        auto* tile = new QToolButton(gridWidget_);
        tile->setFocusPolicy(Qt::StrongFocus);
        tile->setFixedSize(72, 62);
        tile->setIconSize(QSize(32, 32));
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setContextMenuPolicy(Qt::CustomContextMenu);
        tile->setProperty("slot", i);
        tile->setProperty("empty", true);
        tile->installEventFilter(this);
        tiles_.append(tile);
        grid_->addWidget(tile, i / columns, i % columns);
        connect(tile, &QToolButton::clicked, this, [this, i] {
            selected_ = i;
            activateItem(itemAt(i));
        });
        connect(tile, &QWidget::customContextMenuRequested, this, [this, i, tile](QPoint p) {
            selected_ = i;
            contextMenu(i, tile->mapToGlobal(p));
        });
    }
    applyTileStyle();
    page_ = first / pageSize();
    selected_ = qMin(selected_, pageSize() - 1);
    // 72 × 62 tiles, 4 px gaps, 9/8 px grid insets, 30 px category bar:
    // 4 × 7 gives the 318 × 504 body of the original HLaunch screenshot.
    const QSize size(18 + columns * 72 + (columns - 1) * 4,
                     Theme::headerHeight + 16 + rows * 62 + (rows - 1) * 4 + 30);
    desiredSize_ = size;
    gridWidget_->setFixedSize(size.width(), size.height() - Theme::headerHeight - 30);
    setFixedSize(size);
    searchPopup_->setFixedWidth(size.width());
    if (isVisible()) {
        // Stay on the panel's screen after growing.
        showNear(pos(), false);
        if (searchPopup_->isVisible())
            positionSearch();
    }
    refresh();
}
void LauncherPanel::refresh() {
    const auto& catalog = service_.catalog();
    page_ = std::clamp(page_, 0, lastPage());
    selected_ = std::clamp(selected_, 0, visibleSlotCount() - 1);
    if ((!selectionPending_ && persistedCategory_ != catalog.selectedCategory) ||
        catalog.categoryIndex(displayedCategory_) < 0)
        displayedCategory_ = catalog.selectedCategory;
    persistedCategory_ = catalog.selectedCategory;
    for (auto it = icons_.begin(); it != icons_.end();)
        if (!catalog.items.contains(it.key()))
            it = icons_.erase(it);
        else
            ++it;
    for (auto it = pendingIcons_.begin(); it != pendingIcons_.end();)
        if (!catalog.items.contains(it.key()))
            it = pendingIcons_.erase(it);
        else
            ++it;
    syncTabs();
    QStringList visibleIcons;
    for (int i = 0; i < visibleSlotCount(); ++i)
        if (const auto id = itemAt(i); !id.isEmpty())
            visibleIcons.append(id);
    Q_EMIT iconPriorityChanged(visibleIcons);
    for (int i = 0; i < tiles_.size(); ++i) {
        auto* tile = tiles_[i];
        const auto id = itemAt(i);
        const bool empty = id.isEmpty() || !catalog.items.contains(id);
        if (tile->property("empty").toBool() != empty) {
            tile->setProperty("empty", empty);
            tile->style()->unpolish(tile);
            tile->style()->polish(tile);
            tile->update();
        }
        tile->setIcon({});
        tile->setText({});
        tile->setToolTip(tx(u"空槽 · 右键添加"));
        tile->setAccessibleName(tx(u"空槽 %1").arg(absoluteSlot(i) + 1));
        tile->setEnabled(i < visibleSlotCount());
        if (!tile->isEnabled()) {
            tile->setToolTip({});
            tile->setAccessibleName({});
            continue;
        }
        if (empty)
            continue;
        const auto& item = catalog.items[id];
        tile->setText(tile->fontMetrics().elidedText(item.name, Qt::ElideRight, 68));
        tile->setAccessibleName(item.name);
        tile->setToolTip(item.name + u'\n' + item.target);
        tile->setIcon(icons_.contains(id) ? icons_[id].second : theme_.icon(Symbol::Files));
        ensureIcon(item);
    }
    setWindowTitle(tx(u"MT · 启动面板 · 第 %1 页").arg(page_ + 1));
    updateHoverTitle();
    if (searchPopup_->isVisible())
        updateSearch(true);
}
void LauncherPanel::applyTileStyle() {
    // Occupied slots keep the shared card surface; empty slots remain visible
    // without competing with application icons. Unchanged refreshes do not repolish.
    const auto style = QStringLiteral("QToolButton { background:%1; border:1px solid transparent; "
                                      "border-radius:6px; padding:3px 1px; font-size:12px; } "
                                      "QToolButton[empty=\"true\"] { background:transparent; "
                                      "border:1px dashed %5; } "
                                      "QToolButton:hover { background:%1; border:1px solid %2; } "
                                      "QToolButton[empty=\"true\"]:hover { background:transparent; "
                                      "border:1px dashed %2; } QToolButton:pressed, "
                                      "QToolButton[empty=\"true\"]:pressed { background:%3; } "
                                      "QToolButton:focus, QToolButton[empty=\"true\"]:focus "
                                      "{ border:1px solid %4; } QToolButton:disabled, "
                                      "QToolButton[empty=\"true\"]:disabled { background:transparent; "
                                      "border-color:transparent; }")
                           .arg(theme_.color(QStringLiteral("surface-subtle")).name(),
                                theme_.color(QStringLiteral("border-strong")).name(),
                                theme_.color(QStringLiteral("surface-selected")).name(),
                                theme_.color(QStringLiteral("focus")).name(),
                                theme_.color(QStringLiteral("border")).name());
    for (auto* tile : tiles_)
        tile->setStyleSheet(style);
}
void LauncherPanel::clearTilePresses() {
    for (auto* tile : tiles_)
        tile->setDown(false);
}
void LauncherPanel::updateHoverTitle() {
    QString name;
    if (hoveredTile_ >= 0 && hoveredTile_ < visibleSlotCount()) {
        const auto& items = service_.catalog().items;
        const auto it = items.constFind(itemAt(hoveredTile_));
        if (it != items.cend())
            name = it->name;
    }
    hoverTitle_->setText(hoverTitle_->fontMetrics().elidedText(
        name, Qt::ElideRight, hoverTitle_->contentsRect().width()));
    hoverTitle_->setToolTip(name);
    hoverTitle_->setAccessibleName(name);
}
void LauncherPanel::syncTabs() {
    // A saved snapshot may be older than the drag or an already queued order.
    if (tabInteraction_ || tabOrderDirty_ || tabOrderPending_)
        return;
    const auto& catalog = service_.catalog();
    const QSignalBlocker blocker(tabs_);
    for (int i = tabs_->count() - 1; i >= 0; --i)
        if (catalog.categoryIndex(tabs_->tabData(i).toString()) < 0)
            tabs_->removeTab(i);
    for (int i = 0; i < catalog.categories.size(); ++i) {
        const auto& category = catalog.categories[i];
        int existing = i;
        while (existing < tabs_->count() && tabs_->tabData(existing).toString() != category.id)
            ++existing;
        if (existing == tabs_->count()) {
            tabs_->insertTab(i, category.name);
            tabs_->setTabData(i, category.id);
        } else if (existing != i) {
            tabs_->moveTab(existing, i);
        }
        if (tabs_->tabText(i) != category.name)
            tabs_->setTabText(i, category.name);
    }
    const int current = catalog.categoryIndex(categoryId());
    if (tabs_->currentIndex() != current)
        tabs_->setCurrentIndex(current);
}
void LauncherPanel::finishTabInteraction() {
    ++tabInteractionGeneration_;
    tabInteraction_ = false;
    commitTabOrder();
    syncTabs();
}
void LauncherPanel::commitTabOrder() {
    if (tabInteraction_ || !tabOrderDirty_)
        return;
    QStringList order;
    for (int i = 0; i < tabs_->count(); ++i)
        order.append(tabs_->tabData(i).toString());
    tabOrderDirty_ = false;
    tabOrderPending_ = true;
    const auto generation = ++tabOrderGeneration_;
    service_.reorderCategories(
        std::move(order), [guard = QPointer<LauncherPanel>(this), generation](auto) {
            if (!guard || generation != guard->tabOrderGeneration_)
                return;
            guard->tabOrderPending_ = false;
            guard->syncTabs(); // success or failure: use the latest committed order
        });
}
int LauncherPanel::visibleSlotCount() const {
    return (std::min)(pageSize(), LauncherCatalog::maxSlots - page_ * pageSize());
}
void LauncherPanel::focusSelected() {
    if (tiles_.isEmpty())
        return;
    selected_ = std::clamp(selected_, 0, visibleSlotCount() - 1);
    tiles_[selected_]->setFocus();
    gridScroll_->ensureWidgetVisible(tiles_[selected_]);
}
void LauncherPanel::changePage(int direction) {
    const int next = std::clamp(page_ + direction, 0, lastPage());
    if (next == page_)
        return;
    page_ = next;
    refresh();
    focusSelected();
}
void LauncherPanel::ensureIcon(const LaunchItem& item) {
    if (!iconsReady_)
        return;
    const auto source = item.iconPath.isEmpty() ? item.target : item.iconPath;
    if ((icons_.contains(item.id) && icons_[item.id].first == source) ||
        pendingIcons_.value(item.id) == source)
        return;
    pendingIcons_.insert(item.id, source);
    Q_EMIT iconRequested(item.id, source);
}
void LauncherPanel::acceptIcon(const QString& id, const QString& source, const QImage& image) {
    if (pendingIcons_.value(id) != source)
        return;
    pendingIcons_.remove(id);
    const auto current = service_.catalog().items.value(id);
    if (current.id.isEmpty())
        return;
    if ((current.iconPath.isEmpty() ? current.target : current.iconPath) != source) {
        refresh();
        return;
    }
    // A transient extraction failure can be retried on the next refresh/show.
    if (image.isNull())
        return;
    icons_[id] = {source, QIcon(QPixmap::fromImage(image))};
    // Decoration completion must not reset selection, scrolling or tab dragging.
    for (int i = 0; i < tiles_.size(); ++i)
        if (itemAt(i) == id)
            tiles_[i]->setIcon(icons_[id].second);
    for (int row = 0; row < results_->count(); ++row)
        if (results_->item(row)->data(Qt::UserRole).toString() == id)
            results_->item(row)->setIcon(icons_[id].second);
}
void LauncherPanel::activateItem(const QString& id) {
    if (id.isEmpty())
        return;
    const auto item = service_.catalog().items.value(id);
    if (item.id.isEmpty())
        return;
    launchCopy(item);
}
void LauncherPanel::launchCopy(const LaunchItem& item) {
    Q_EMIT launchRequested(item);
    if (!keepOpen)
        hide();
    else
        searchPopup_->hide();
}
void LauncherPanel::launchCategory(const QString& category) {
    const auto& c = service_.catalog();
    const int index = c.categoryIndex(category);
    if (index < 0)
        return;
    // Each launch is independent; one failure does not stop the rest.
    for (const auto& id : c.categories[index].slots)
        if (!id.isEmpty())
            Q_EMIT launchRequested(c.items.value(id));
    if (!keepOpen)
        hide();
}
void LauncherPanel::deleteItem(int index) {
    if (itemAt(index).isEmpty())
        return;
    if (QMessageBox::question(this, tx(u"删除启动项"), tx(u"移除此启动项？原文件不会删除。"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) == QMessageBox::Yes)
        service_.erase(categoryId(), absoluteSlot(index));
}
void LauncherPanel::addItem() {
    const auto& catalog = service_.catalog();
    const int category = catalog.categoryIndex(categoryId());
    if (category < 0)
        return;
    int slot = absoluteSlot(selected_);
    while (slot < LauncherCatalog::maxSlots &&
           !catalog.categories[category].slots.value(slot).isEmpty())
        ++slot;
    if (slot == LauncherCatalog::maxSlots) {
        QMessageBox::information(this, tx(u"添加启动项"), tx(u"分类已达到容量上限"));
        return;
    }
    page_ = slot / pageSize();
    selected_ = slot % pageSize();
    refresh();
    focusSelected();
    editItem(selected_);
}
void LauncherPanel::editItem(int index) {
    if (index < 0 || index >= visibleSlotCount())
        return;
    const auto category = categoryId();
    const int slot = absoluteSlot(index);
    auto item = service_.catalog().items.value(itemAt(index));
    QDialog dialog(this);
    dialog.setWindowTitle(item.id.isEmpty() ? tx(u"添加启动项") : tx(u"编辑启动项"));
    dialog.setMinimumWidth(450);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(item.name, &dialog);
    auto* target = new QLineEdit(item.target, &dialog);
    auto* arguments = new QLineEdit(item.arguments, &dialog);
    auto* directory = new QLineEdit(item.workingDirectory, &dialog);
    auto* icon = new QLineEdit(item.iconPath, &dialog);
    auto* elevated = new QCheckBox(tx(u"以管理员身份运行"), &dialog);
    elevated->setChecked(item.elevated);
    form->addRow(tx(u"名称"), name);
    auto* targetRow = new QHBoxLayout;
    targetRow->addWidget(target);
    auto* browse = new QPushButton(tx(u"浏览"), &dialog);
    targetRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tx(u"选择程序或文件"));
        if (!path.isEmpty()) {
            target->setText(path);
            if (name->text().isEmpty())
                name->setText(QFileInfo(path).completeBaseName());
        }
    });
    form->addRow(tx(u"路径或 URL"), targetRow);
    form->addRow(tx(u"参数"), arguments);
    form->addRow(tx(u"工作目录"), directory);
    form->addRow(tx(u"图标来源"), icon);
    form->addRow(elevated);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        item.name = name->text().trimmed();
        item.target = target->text().trimmed();
        item.arguments = arguments->text();
        item.workingDirectory = directory->text();
        item.iconPath = icon->text();
        item.elevated = elevated->isChecked();
        buttons->setEnabled(false);
        service_.put(category, slot, item,
                     [guard = QPointer<QDialog>(&dialog), buttons](auto result) {
                         if (!guard)
                             return;
                         buttons->setEnabled(true);
                         if (result)
                             guard->accept();
                     });
    });
    dialog.exec();
}
void LauncherPanel::contextMenu(int index, QPoint point) {
    if (index < 0 || index >= visibleSlotCount())
        return;
    QMenu menu(this);
    const auto id = itemAt(index);
    const auto item = service_.catalog().items.value(id);
    if (item.id.isEmpty()) {
        menu.addAction(tx(u"添加项目（Insert）"), this, [this, index] { editItem(index); });
        menu.addAction(tx(u"在此插入空槽"), this,
                       [this, index] { service_.insertBlank(categoryId(), absoluteSlot(index)); });
        menu.exec(point);
        return;
    }
    menu.addAction(tx(u"打开"), this, [this, item] { launchCopy(item); });
    // Elevation applies to this launch only; the item keeps its own setting.
    menu.addAction(tx(u"以管理员身份运行"), this, [this, item] {
        auto once = item;
        once.elevated = true;
        launchCopy(once);
    });
    menu.addSeparator();
    if (!LauncherCatalog::isUrl(item.target))
        menu.addAction(tx(u"打开文件所在位置"), this,
                       [this, item] { Q_EMIT revealRequested(item.target); });
    const auto copy = [](const QString& text) { QGuiApplication::clipboard()->setText(text); };
    menu.addAction(tx(u"复制名称"), this, [copy, item] { copy(item.name); });
    menu.addAction(tx(u"复制目标"), this, [copy, item] { copy(item.target); });
    menu.addAction(tx(u"复制完整命令"), this,
                   [copy, item] { copy(LauncherCatalog::commandLine(item)); });
    menu.addSeparator();
    menu.addAction(tx(u"在此插入空槽"), this,
                   [this, index] { service_.insertBlank(categoryId(), absoluteSlot(index)); });
    auto* moveMenu = menu.addMenu(tx(u"移动到分类"));
    for (const auto& category : service_.catalog().categories) {
        auto* action = moveMenu->addAction(category.name, this, [this, id, target = category.id] {
            service_.moveToCategory(id, target,
                                    [guard = QPointer<LauncherPanel>(this), target](auto result) {
                                        if (guard && result)
                                            guard->service_.selectCategory(target);
                                    });
        });
        // The current category is shown but cannot be chosen.
        const bool current = category.id == categoryId();
        action->setCheckable(current);
        action->setChecked(current);
        action->setEnabled(!current);
    }
    menu.addSeparator();
    menu.addAction(tx(u"删除（Delete）"), this, [this, index] { deleteItem(index); });
    menu.addAction(tx(u"属性（F2）"), this, [this, index] { editItem(index); });
    menu.exec(point);
}
void LauncherPanel::categoryMenu(QPoint point) {
    const int index = tabs_->tabAt(point);
    const auto id = index >= 0 ? tabs_->tabData(index).toString() : categoryId();
    const auto& catalog = service_.catalog();
    const int position = catalog.categoryIndex(id);
    if (position < 0)
        return;
    int count = 0;
    for (const auto& slot : catalog.categories[position].slots)
        count += slot.isEmpty() ? 0 : 1;
    QMenu menu(this);
    auto* launchAll =
        menu.addAction(tx(u"启动页面中的所有项目"), this, [this, id] { launchCategory(id); });
    launchAll->setEnabled(count > 0);
    menu.addSeparator();
    menu.addAction(tx(u"新增分类"), this, [this] {
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, tx(u"新增分类"), tx(u"名称"), QLineEdit::Normal, {}, &ok);
        if (ok)
            service_.addCategory(name);
    });
    menu.addAction(tx(u"重命名分类"), this, [this, id] {
        const auto& c = service_.catalog();
        const int i = c.categoryIndex(id);
        if (i < 0)
            return;
        bool ok = false;
        const auto name = QInputDialog::getText(this, tx(u"分类名称"), tx(u"名称"),
                                                QLineEdit::Normal, c.categories[i].name, &ok);
        if (ok)
            service_.renameCategory(id, name);
    });
    auto* left =
        menu.addAction(tx(u"左移"), this, [this, id] { service_.reorderCategory(id, -1); });
    left->setEnabled(position > 0);
    auto* right =
        menu.addAction(tx(u"右移"), this, [this, id] { service_.reorderCategory(id, 1); });
    right->setEnabled(position + 1 < catalog.categories.size());
    menu.addSeparator();
    auto* remove = menu.addAction(tx(u"删除分类"), this, [this, id, position, count] {
        // Items are never lost with their category; they join the neighbour.
        const auto destination =
            service_.catalog().categories.value(position > 0 ? position - 1 : 1).name;
        if (count > 0 &&
            QMessageBox::question(
                this, tx(u"删除分类"),
                tx(u"删除此分类？其中的 %1 个启动项将移到「%2」。").arg(count).arg(destination),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        service_.removeCategory(id);
    });
    remove->setEnabled(catalog.categories.size() > 1);
    menu.exec(tabs_->mapToGlobal(point));
}
void LauncherPanel::updateTabArrows() {
    const bool overflow =
        qtRight_ && qtLeft_ && (qtLeft_->isVisibleTo(tabs_) || qtRight_->isVisibleTo(tabs_));
    tabLeft_->setVisible(overflow);
    tabRight_->setVisible(overflow);
    tabLeft_->setEnabled(overflow && qtLeft_->isEnabled());
    tabRight_->setEnabled(overflow && qtRight_->isEnabled());
}
void LauncherPanel::changeCategory(int direction) {
    const int count = tabs_->count();
    if (count)
        tabs_->setCurrentIndex((tabs_->currentIndex() + direction + count) % count);
}
void LauncherPanel::showSearch(const QString& initial) {
    search_->setText(initial);
    searchPopup_->show();
    positionSearch();
    searchPopup_->activateWindow();
    search_->setFocus();
    updateSearch();
}
void LauncherPanel::positionSearch() {
    auto point = pos() + QPoint(0, height() + 6);
    if (auto* currentScreen = screen()) {
        const auto area = currentScreen->availableGeometry();
        if (point.y() + searchPopup_->height() > area.bottom() + 1)
            point.setY(pos().y() - searchPopup_->height() - 6);
        point.setX(std::clamp(point.x(), area.left(),
                              std::max(area.left(), area.right() + 1 - searchPopup_->width())));
        point.setY(std::clamp(point.y(), area.top(),
                              std::max(area.top(), area.bottom() + 1 - searchPopup_->height())));
    }
    searchPopup_->move(point);
}
void LauncherPanel::updateSearch(bool preserveSelection) {
    const auto selected = preserveSelection && results_->currentItem()
                              ? results_->currentItem()->data(Qt::UserRole).toString()
                              : QString{};
    const int scroll = preserveSelection ? results_->verticalScrollBar()->value() : 0;
    results_->clear();
    const auto& c = service_.catalog();
    const auto matches = c.search(search_->text());
    for (const auto& match : matches) {
        const auto& item = c.items[match.itemId];
        const auto& category = c.categories[c.categoryIndex(match.categoryId)];
        auto* row =
            new QListWidgetItem(item.name + QStringLiteral("  ·  ") + category.name, results_);
        row->setData(Qt::UserRole, item.id);
        row->setIcon(icons_.contains(item.id) ? icons_[item.id].second : theme_.icon(Symbol::Files));
        row->setSizeHint(QSize(0, 32));
        row->setToolTip(item.name + u'\n' + category.name + u'\n' + item.target);
    }
    const bool hasQuery = !search_->text().isEmpty();
    results_->setFixedHeight(std::min(210, results_->count() * 36 + 4));
    results_->setVisible(hasQuery && !matches.isEmpty());
    searchEmpty_->setVisible(hasQuery && matches.isEmpty());
    if (results_->count())
        results_->setCurrentRow(0);
    for (int i = 0; i < results_->count(); ++i)
        if (results_->item(i)->data(Qt::UserRole).toString() == selected)
            results_->setCurrentRow(i);
    results_->verticalScrollBar()->setValue(scroll);
    searchPopup_->adjustSize();
    positionSearch();
    QTimer::singleShot(0, this, &LauncherPanel::requestSearchIcons);
}
void LauncherPanel::requestSearchIcons() {
    if (!results_->isVisible() || !results_->count())
        return;
    const int first = qMax(0, results_->indexAt(QPoint(1, 1)).row());
    int last = results_->indexAt(results_->viewport()->rect().bottomRight() - QPoint(1, 1)).row();
    if (last < first)
        last = qMin(results_->count() - 1, first + 7);
    QStringList visible;
    for (int row = first; row <= last; ++row)
        visible.append(results_->item(row)->data(Qt::UserRole).toString());
    Q_EMIT iconPriorityChanged(visible);
    for (const auto& id : visible) {
        const auto it = service_.catalog().items.constFind(id);
        if (it != service_.catalog().items.cend())
            ensureIcon(it.value());
    }
}
void LauncherPanel::keyPressEvent(QKeyEvent* event) {
    const auto key = event->key();
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (key == Qt::Key_Escape) {
        if (searchPopup_->isVisible()) {
            if (!search_->text().isEmpty())
                search_->clear();
            else {
                searchPopup_->hide();
                activateWindow();
                focusSelected();
            }
        } else
            hide();
    } else if (key == Qt::Key_Tab || key == Qt::Key_Backtab)
        changeCategory(key == Qt::Key_Backtab || event->modifiers() & Qt::ShiftModifier ? -1 : 1);
    else if (key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
        changePage(key == Qt::Key_PageDown ? 1 : -1);
    } else if (key == Qt::Key_Insert)
        addItem();
    else if (key == Qt::Key_F2)
        editItem(selected_);
    else if (key == Qt::Key_Delete)
        deleteItem(selected_);
    else if (key == Qt::Key_Home || key == Qt::Key_End) {
        // First / last real item on this page.
        int target = -1;
        for (int i = 0; i < tiles_.size(); ++i)
            if (!itemAt(i).isEmpty() && (target < 0 || key == Qt::Key_End))
                target = i;
        if (target >= 0) {
            selected_ = target;
            tiles_[selected_]->setFocus();
            gridScroll_->ensureWidgetVisible(tiles_[selected_]);
        }
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter)
        activateItem(itemAt(selected_));
    else if (ctrl && key == Qt::Key_Space) {
        setPinned(!keepOpen);
        Q_EMIT pinnedChanged(keepOpen);
    } else if (ctrl && key == Qt::Key_F)
        showSearch();
    else if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left ||
             key == Qt::Key_Right) {
        const int delta = key == Qt::Key_Up     ? -columns_
                          : key == Qt::Key_Down ? columns_
                          : key == Qt::Key_Left ? -1
                                                : 1;
        selected_ = std::clamp(selected_ + delta, 0, visibleSlotCount() - 1);
        focusSelected();
    } else if (!ctrl && !event->text().isEmpty() && event->text().front().isPrint())
        showSearch(event->text());
    else {
        Panel::keyPressEvent(event);
        return;
    }
    event->accept();
}
bool LauncherPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == hoverTitle_ && event->type() == QEvent::Resize)
        updateHoverTitle();
    if (watched == tabs_) {
        const bool blankPress = event->type() == QEvent::MouseButtonPress &&
            tabs_->tabAt(static_cast<QMouseEvent*>(event)->position().toPoint()) < 0;
        if (handleSurfaceDrag(tabs_, event, blankPress))
            return true;
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            ++tabInteractionGeneration_;
            tabInteraction_ = true;
        }
        if (event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            // QTabBar must finish its own release before we persist the final order.
            const auto generation = tabInteractionGeneration_;
            QTimer::singleShot(0, this, [this, generation] {
                if (generation == tabInteractionGeneration_)
                    finishTabInteraction();
            });
        }
    }
    if (watched == searchPopup_ && event->type() == QEvent::WindowDeactivate)
        checkFocusLater();
    if (watched && (watched == qtLeft_ || watched == qtRight_)) {
        const auto type = event->type();
        if (type == QEvent::Show || type == QEvent::Hide || type == QEvent::EnabledChange)
            QTimer::singleShot(0, this, &LauncherPanel::updateTabArrows);
        return false;
    }
    if (watched == search_ || watched == results_) {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                keyPressEvent(key);
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                if (auto* row = results_->currentItem())
                    activateItem(row->data(Qt::UserRole).toString());
                return true;
            }
            if (watched == search_ && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
                if (results_->count())
                    results_->setCurrentRow(
                        qBound(0, results_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1),
                               results_->count() - 1));
                return true;
            }
        }
        return false;
    }
    const int index = tiles_.indexOf(qobject_cast<QToolButton*>(watched));
    if (index >= 0) {
        if (event->type() == QEvent::Enter) {
            hoveredTile_ = index;
            updateHoverTitle();
        } else if (event->type() == QEvent::Leave && hoveredTile_ == index) {
            hoveredTile_ = -1;
            updateHoverTitle();
        }
        if (event->type() == QEvent::UngrabMouse)
            tiles_[index]->setDown(false);
        const bool empty = itemAt(index).isEmpty();
        if (empty && event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            selected_ = index;
            tiles_[index]->setFocus(Qt::MouseFocusReason);
        }
        if (handleSurfaceDrag(tiles_[index], event, empty))
            return true;
        if (event->type() == QEvent::MouseButtonPress) {
            selected_ = index;
            dragStart_ = static_cast<QMouseEvent*>(event)->position().toPoint();
        }
        if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->buttons() & Qt::LeftButton) &&
                (mouse->position().toPoint() - dragStart_).manhattanLength() >
                    QApplication::startDragDistance() &&
                !itemAt(index).isEmpty()) {
                const QPointer<QToolButton> source(tiles_[index]);
                // Native drag-and-drop owns the release; it will not reliably
                // reach the button that handled the original press.
                clearTilePresses();
                auto* drag = new QDrag(source.data());
                auto* mime = new QMimeData;
                mime->setData(mimeType, itemAt(index).toUtf8());
                drag->setMimeData(mime);
                drag->setPixmap(source->icon().pixmap(32, 32));
                drag->exec(Qt::MoveAction);
                if (source)
                    source->setDown(false);
                return true;
            }
        }
        if (event->type() == QEvent::FocusIn)
            selected_ = index;
        if (event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        return false;
    }
    return Panel::eventFilter(watched, event);
}
void LauncherPanel::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(mimeType) || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void LauncherPanel::dragMoveEvent(QDragMoveEvent* e) {
    const auto point = e->position().toPoint();
    const auto tabPoint = tabs_->mapFrom(this, point);
    if (tabs_->rect().contains(tabPoint)) {
        const int tab = tabs_->tabAt(tabPoint);
        if (tab >= 0)
            tabs_->setCurrentIndex(tab);
    }
    e->acceptProposedAction();
}
void LauncherPanel::dropEvent(QDropEvent* e) {
    clearTilePresses();
    int slot = -1;
    const auto p = e->position().toPoint();
    if (!gridScroll_->viewport()->rect().contains(gridScroll_->viewport()->mapFrom(this, p))) {
        e->ignore();
        return;
    }
    for (int i = 0; i < visibleSlotCount(); ++i)
        if (QRect(tiles_[i]->mapTo(this, QPoint{}), tiles_[i]->size()).contains(p)) {
            slot = i;
            break;
        }
    if (slot < 0) {
        e->ignore();
        return;
    }
    const auto onto = service_.catalog().items.value(itemAt(slot));
    const auto urls = e->mimeData()->urls();
    if (e->mimeData()->hasFormat(mimeType)) {
        if (!service_.move(QString::fromUtf8(e->mimeData()->data(mimeType)), categoryId(),
                           absoluteSlot(slot))) {
            e->ignore();
            return;
        }
        selected_ = slot;
        focusSelected();
    } else if (!onto.id.isEmpty() && LauncherCatalog::acceptsFileArguments(onto) && !urls.isEmpty() &&
             std::ranges::all_of(urls, [](const QUrl& url) { return url.isLocalFile(); })) {
        // Files dropped on a program open with it once, after its own arguments.
        auto once = onto;
        QStringList arguments;
        if (!once.arguments.trimmed().isEmpty())
            arguments << once.arguments.trimmed();
        for (const auto& url : urls)
            arguments << LauncherCatalog::quoteArgument(
                QDir::toNativeSeparators(url.toLocalFile()));
        once.arguments = arguments.join(u' ');
        launchCopy(once);
    } else {

        QStringList paths;
        for (const auto& url : e->mimeData()->urls())
            paths.append(url.isLocalFile() ? url.toLocalFile() : url.toString());
        Q_EMIT importRequested(paths, categoryId(), absoluteSlot(slot));
    }
    e->acceptProposedAction();
}
void LauncherPanel::moveEvent(QMoveEvent* e) {
    Panel::moveEvent(e);
    if (searchPopup_ && searchPopup_->isVisible())
        positionSearch();
}
void LauncherPanel::hideEvent(QHideEvent* e) {
    hoveredTile_ = -1;
    updateHoverTitle();
    clearTilePresses();
    finishTabInteraction();
    if (searchPopup_)
        searchPopup_->hide();
    Panel::hideEvent(e);
}
void LauncherPanel::wheelEvent(QWheelEvent* e) {
    changeCategory(e->angleDelta().y() < 0 ? 1 : -1);
    e->accept();
}
bool LauncherPanel::event(QEvent* e) {
    if (e->type() == QEvent::WindowDeactivate)
        checkFocusLater();
    return Panel::event(e);
}
void LauncherPanel::checkFocusLater() {
    QTimer::singleShot(100, this, [this] {
        const auto owned = [this](QWidget* widget) {
            // Owned tool windows/dialogs are separate windows; QWidget's visual
            // ancestry check does not describe this window ownership group.
            for (auto* current = widget; current; current = current->parentWidget())
                if (current == this)
                    return true;
            return false;
        };
        if (isVisible() && !keepOpen && !owned(QApplication::activeWindow()) &&
            !owned(QApplication::activeModalWidget()) && !owned(QApplication::activePopupWidget()))
            hide();
    });
}
void LauncherPanel::retranslate() {
    const auto label = [](QToolButton* button, const QString& text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    label(menuButton_, tx(u"启动器菜单"));
    label(pin_, tx(u"保持打开（Ctrl+Space）"));
    label(closeButton_, tx(u"隐藏启动面板（Esc）"));
    label(tabLeft_, tx(u"向左滚动分类"));
    label(tabRight_, tx(u"向右滚动分类"));
    search_->setPlaceholderText(tx(u"搜索所有分类"));
    search_->setAccessibleName(tx(u"搜索启动项"));
    searchEmpty_->setText(tx(u"没有匹配的启动项\n换个关键词试试"));
    searchPopup_->setWindowTitle(tx(u"MT · 搜索启动项"));
    results_->setAccessibleName(tx(u"跨分类搜索结果"));
    refresh();
}
void LauncherPanel::fitToWorkArea(const QRect& area) {
    setFixedSize(desiredSize_.boundedTo(area.size()));
    searchPopup_->setFixedWidth(width());
}
void LauncherPanel::showNear(QPoint anchor, bool activate) {
    auto* target = QGuiApplication::screenAt(anchor);
    if (!target)
        target = QGuiApplication::primaryScreen();
    if (target)
        fitToWorkArea(target->availableGeometry());
    refresh();
    Panel::showNear(anchor, activate);
    if (activate)
        focusSelected();
}
void LauncherPanel::showCentered(QPoint center, bool activate) {
    auto* target = QGuiApplication::screenAt(center);
    if (!target)
        target = QGuiApplication::primaryScreen();
    if (target)
        fitToWorkArea(target->availableGeometry());
    auto anchor = center - QPoint(width() / 2, height() / 2);
    if (target) {
        // Clamp on the cursor's screen before showNear resolves the top-left.
        const auto area = target->availableGeometry();
        anchor.setX(qBound(area.left(), anchor.x(), qMax(area.left(), area.right() - width() + 1)));
        anchor.setY(qBound(area.top(), anchor.y(), qMax(area.top(), area.bottom() - height() + 1)));
    }
    showNear(anchor, activate);
}
} // namespace mt
