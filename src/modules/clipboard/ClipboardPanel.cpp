#include "ClipboardPanel.h"
#include "ClipboardPreview.h"
#include "ClipboardText.h"
#include "ClipboardThumbnailCache.h"
#include "infrastructure/i18n/I18n.h"
#include <QApplication>
#include <QPainterPath>
#include "ui/components/Theme.h"
#include <QAbstractListModel>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>
#include <QCursor>
#include <array>
#include <algorithm>
#include <utility>

namespace mt {
namespace {
class RecordsModel final : public QAbstractListModel {
  public:
    ClipboardRecords entries;
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= entries.size())
            return {};
        if (role == Qt::UserRole)
            return QVariant::fromValue(entries[index.row()]);
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole)
            return entries[index.row()].sensitive ? tx(u"敏感内容")
                                                  : entries[index.row()].summary().left(500);
        return {};
    }
    void replace(ClipboardRecords rows) {
        beginResetModel();
        entries = std::move(rows);
        endResetModel();
    }
};
struct ClipCardLayout {
    QRect card;
    QRect content;
    QRect footer;
    std::array<QRect, 3> actions;
    explicit ClipCardLayout(QRect row) {
        card = row.adjusted(6, 3, -6, -3);
        content = card.adjusted(12, 10, -12, -30);
        footer = QRect(content.left(), card.bottom() - 25, content.width(), 18);
        const int start = content.right() - 65;
        for (int i = 0; i < 3; ++i)
            actions[i] = QRect(start + i * 22, content.top(), 22, 20);
    }
    int actionAt(QPoint point) const {
        for (int i = 0; i < 3; ++i)
            if (actions[i].contains(point))
                return i;
        return -1;
    }
};
// Side-button momentum: each press adds velocity that eases out, so a burst of
// presses becomes one smooth continuous scroll (the buttons report no hold).
constexpr qreal sideKick = 18.0;     // px per frame added per press
constexpr qreal sideMaximum = 60.0;  // px per frame
constexpr qreal sideFriction = 0.90; // per-frame decay
constexpr qreal sideStop = 0.4;
Symbol clipSymbol(ClipKind kind) {
    if (kind == ClipKind::Image)
        return Symbol::Image;
    if (kind == ClipKind::Files)
        return Symbol::Files;
    return Symbol::Text;
}
QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
class CardDelegate final : public QStyledItemDelegate {
  public:
    Theme& theme;
    bool masked = true;
    bool animated = false;
    int hoverRow = -1;
    int hoverAction = -1;
    QHash<int, qreal> levels; // per-row hover fill, eased toward hoverRow
    QTimer tick;
    QPointer<QWidget> viewport;
    mutable ClipboardThumbnailCache thumbnails;
    CardDelegate(Theme& t, ClipboardPanel* p) : QStyledItemDelegate(p), theme(t) {
        tick.setInterval(16);
        QObject::connect(&tick, &QTimer::timeout, this, [this] { step(); });
    }
    void setHover(int row, int action) {
        if (row == hoverRow && action == hoverAction)
            return;
        hoverRow = row;
        hoverAction = action;
        if (!animated) {
            levels.clear();
            if (row >= 0)
                levels[row] = 1.0;
        } else if (!tick.isActive()) {
            tick.start();
        }
        if (viewport)
            viewport->update();
    }
    void reset() {
        hoverRow = hoverAction = -1;
        levels.clear();
        tick.stop();
    }
    void step() {
        const qreal delta = 16.0 / Theme::motionMs;
        bool moving = false;
        if (hoverRow >= 0 && !levels.contains(hoverRow))
            levels[hoverRow] = 0.0;
        for (auto i = levels.begin(); i != levels.end();) {
            const qreal target = i.key() == hoverRow ? 1.0 : 0.0;
            i.value() = target > i.value() ? qMin(target, i.value() + delta)
                                           : qMax(target, i.value() - delta);
            moving |= i.value() != target;
            i = i.value() <= 0.0 && target == 0.0 ? levels.erase(i) : std::next(i);
        }
        if (!moving)
            tick.stop();
        if (viewport)
            viewport->update();
    }
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const auto record = index.data(Qt::UserRole).value<ClipboardRecord>();
        return QSize(330,
                     record.kind == ClipKind::Text || record.kind == ClipKind::Html ? 86 : 104);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto record = index.data(Qt::UserRole).value<ClipboardRecord>();
        const ClipCardLayout layout(option.rect);
        const bool selected = option.state & QStyle::State_Selected;
        const qreal hover = levels.value(index.row(), 0.0);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // Soft filled cards without a resting outline; selection uses the shared
        // selection fill plus a short accent pill instead of a bright frame.
        const auto rest = theme.color(QStringLiteral("surface-subtle"));
        const auto fill = selected ? theme.color(QStringLiteral("surface-selected"))
                                   : mix(rest, theme.color(QStringLiteral("surface-hover")), hover);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(layout.card), 8, 8);
        if (selected) {
            painter->setBrush(theme.color(QStringLiteral("accent")));
            painter->drawRoundedRect(
                QRectF(layout.card.left() + 3, layout.card.center().y() - 9, 3, 18), 1.5, 1.5);
        }

        const Symbol actions[]{Symbol::Star, Symbol::PinToTop, Symbol::Trash};
        const bool rowHot = index.row() == hoverRow;
        for (int i = 0; i < 3; ++i) {
            const bool active = (i == 0 && record.favorite) || (i == 1 && record.pinned);
            const bool hot = rowHot && hoverAction == i;
            if (hot) {
                painter->setBrush(theme.color(QStringLiteral("active-bg")));
                painter->drawRoundedRect(QRectF(layout.actions[i]).adjusted(0, -1, 0, 1), 5, 5);
            }
            QRect glyph(QPoint(), QSize(15, 15));
            glyph.moveCenter(layout.actions[i].center());
            // Always visible (DEC-019) but quieter until the card is engaged.
            painter->setOpacity(active || selected || hot ? 1.0 : 0.55 + 0.45 * hover);
            theme.icon(actions[i], active)
                .paint(painter, glyph, Qt::AlignCenter, hot ? QIcon::Active : QIcon::Normal);
            painter->setOpacity(1.0);
        }

        auto content = layout.content;
        content.setRight(layout.actions[0].left() - 8);
        const bool hidden = masked && record.sensitive;
        if (record.kind == ClipKind::Image && !hidden) {
            const auto thumb = thumbnails.pixmap(record, content.size(),
                                                  painter->device()->devicePixelRatioF());
            if (!thumb.isNull())
                painter->drawPixmap(content.topLeft(), thumb);
        } else {
            auto font = option.font;
            font.setPixelSize(14);
            painter->setFont(font);
            painter->setPen(theme.color(QStringLiteral("text")));
            QString text =
                hidden ? tx(u"••••••••  敏感内容") : record.summary().left(1000);
            if (record.kind == ClipKind::Files && !hidden) {
                const auto path = record.files.value(0, record.text.section(u'\n', 0, 0));
                const auto name = QFileInfo(path).fileName();
                text = (name.isEmpty() ? path : name) + u'\n' + path;
            }
            text.replace(u'\n', QChar::LineSeparator);
            // A short colour value gets a swatch in front of the text.
            if (const auto swatch = hidden || record.kind == ClipKind::Files
                                        ? QColor{}
                                        : clipColor(record.text);
                swatch.isValid()) {
                const int size = 14;
                const QRectF chip(content.left(),
                                  content.top() + (painter->fontMetrics().lineSpacing() - size) / 2.0,
                                  size, size);
                QPainterPath path;
                path.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
                painter->fillPath(path, swatch);
                painter->strokePath(path, QPen(theme.color(QStringLiteral("border")), 1));
                content.setLeft(content.left() + size + 7);
            }
            QTextLayout textLayout(text, font);
            QTextOption wrap;
            wrap.setWrapMode(QTextOption::WrapAnywhere);
            textLayout.setTextOption(wrap);
            textLayout.beginLayout();
            qreal y = 0;
            for (int i = 0; i < (record.kind == ClipKind::Files ? 3 : 2); ++i) {
                auto line = textLayout.createLine();
                if (!line.isValid())
                    break;
                line.setLineWidth(content.width());
                line.setPosition(QPointF(0, y));
                y += line.height();
            }
            textLayout.endLayout();
            painter->save();
            painter->setClipRect(content);
            textLayout.draw(painter, content.topLeft());
            painter->restore();
        }

        auto metaFont = option.font;
        metaFont.setPixelSize(11);
        painter->setFont(metaFont);
        const auto number = QStringLiteral("#%1").arg(index.row() + 1);
        const QRect badge(layout.footer.topLeft(),
                          QSize(painter->fontMetrics().horizontalAdvance(number) + 10, 18));
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme.color(QStringLiteral("accent-soft")));
        painter->drawRoundedRect(badge, 4, 4);
        painter->setPen(theme.color(QStringLiteral("accent")));
        painter->drawText(badge, Qt::AlignCenter, number);
        QRect typeIcon(layout.footer.right() - 12, layout.footer.center().y() - 6, 13, 13);
        theme.icon(clipSymbol(record.kind)).paint(painter, typeIcon);
        auto timestamp = layout.footer;
        timestamp.setLeft(badge.right() + 12);
        timestamp.setRight(typeIcon.left() - 10);
        painter->setPen(theme.color(QStringLiteral("muted")));
        painter->drawText(timestamp, Qt::AlignRight | Qt::AlignVCenter, clipRelativeTime(record.captured, QDateTime::currentDateTimeUtc()));
        painter->restore();
    }
};
} // namespace
ClipboardPanel::ClipboardPanel(Theme& theme, bool animateSearch)
    : Panel(theme, tx(u"MT · 剪贴板")), theme_(theme) {
    setFixedSize(380, 504 + Theme::headerHeight);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    // One header row. Searching collapses only the left filter pills and the
    // magnifier while the field grows in their place; More and Close never move.
    toolbar_ = theme.header(this);
    auto* row = static_cast<QHBoxLayout*>(toolbar_->layout());
    browse_ = new QWidget(toolbar_);
    auto* browseRow = new QHBoxLayout(browse_);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(4);
    const Symbol symbols[] = {Symbol::Text, Symbol::Image, Symbol::Files, Symbol::Star};
    const QStringList labels{tx(u"文本 / 富文本"), tx(u"图片"),
                             tx(u"文件"), tx(u"收藏")};
    // As in the original Hopy: the three content types share one pill and
    // favorites sits in its own matching pill.
    const auto group = [this, browseRow] {
        auto* frame = new QFrame(browse_);
        frame->setObjectName(QStringLiteral("FilterGroup"));
        auto* layout = new QHBoxLayout(frame);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(1);
        browseRow->addWidget(frame);
        return layout;
    };
    auto* types = group();
    auto* favorites = group();
    for (int i = 0; i < 4; ++i) {
        auto* button = theme.button(symbols[i], labels[i], browse_, true);
        button->setObjectName(QStringLiteral("FilterButton"));
        button->setFixedSize(24, 24);
        button->setIconSize(QSize(16, 16));
        filters_.append(button);
        (i < 3 ? types : favorites)->addWidget(button);
        connect(button, &QToolButton::clicked, this,
                [this, i] { setFilter(filter_ == i ? -1 : i); });
    }
    row->addWidget(browse_);
    searchBox_ = new QWidget(toolbar_);
    auto* searchRow = new QHBoxLayout(searchBox_);
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(4);
    auto* back = theme.button(Symbol::Back, tx(u"退出搜索（Esc）"), searchBox_);
    searchRow->addWidget(back);
    connect(back, &QToolButton::clicked, this, &ClipboardPanel::closeSearch);
    search_ = new QLineEdit(searchBox_);
    search_->setPlaceholderText(tx(u"搜索历史内容"));
    search_->setAccessibleName(tx(u"搜索历史内容"));
    search_->setClearButtonEnabled(true);
    search_->setFixedHeight(Theme::headerHeight - 2 * Theme::headerInset);
    search_->setMinimumWidth(0);
    searchRow->addWidget(search_, 1);
    search_->installEventFilter(this);
    row->addWidget(searchBox_);
    row->addStretch(1);
    searchButton_ = theme.button(Symbol::Search, tx(u"搜索（/）"), toolbar_);
    row->addWidget(searchButton_);
    connect(searchButton_, &QToolButton::clicked, this, &ClipboardPanel::showSearch);
    auto* more = theme.button(Symbol::More, tx(u"更多操作"), toolbar_);
    row->addWidget(more);
    connect(more, &QToolButton::clicked, this, [this, more] {
        auto& menu = *new QMenu(this);
        connect(&menu, &QMenu::aboutToHide, &menu, &QObject::deleteLater);
        connect(this, &Panel::dismissed, &menu, &QMenu::close);
        menu.addAction(paused_ ? tx(u"恢复记录") : tx(u"暂停记录"), this,
                       [this] { Q_EMIT pauseRequested(!paused_); });
        menu.addAction(tx(u"快捷键说明"), this, &ClipboardPanel::showHelp);
        menu.addAction(tx(u"设置"), this, [this] { Q_EMIT settingsRequested(); });
        menu.popup(more->mapToGlobal(QPoint(0, more->height())));
    });
    auto* close = theme.button(Symbol::Close, tx(u"隐藏剪贴板（Esc）"), toolbar_);
    row->addWidget(close);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    enableTitleDrag(toolbar_);
    browseFade_ = new QGraphicsOpacityEffect(browse_);
    browse_->setGraphicsEffect(browseFade_);
    searchFade_ = new QGraphicsOpacityEffect(searchBox_);
    searchBox_->setGraphicsEffect(searchFade_);
    browseWidth_ = browse_->sizeHint().width();
    searchAnimation_ = new QVariantAnimation(this);
    searchAnimation_->setObjectName(QStringLiteral("ClipboardSearchAnimation"));
    searchAnimation_->setDuration(searchMotionMs);
    searchAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    animateSearch_ = animateSearch;
    connect(searchAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { applySearchProgress(value.toReal()); });
    connect(searchAnimation_, &QVariantAnimation::finished, this,
            [this] { applySearchProgress(searchProgress_); });
    applySearchProgress(0.0);
    layoutColumn()->addWidget(toolbar_);
    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(6, 8, 6, 8);
    bodyLayout->setSpacing(0);
    list_ = new QListView(body);
    list_->setAccessibleName(tx(u"剪贴板历史"));
    list_->setMouseTracking(true);
    list_->setSpacing(0);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    model_ = new RecordsModel(list_);
    list_->setModel(model_);
    auto* delegate = new CardDelegate(theme, this);
    delegate->animated = animateSearch;
    delegate->viewport = list_->viewport();
    list_->setItemDelegate(delegate);
    list_->installEventFilter(this);
    listViewport_ = list_->viewport();
    listViewport_->setMouseTracking(true);
    listViewport_->installEventFilter(this);
    preview_ = new ClipboardPreview(theme, this);
    preview_->setAnimated(animateSearch);
    connect(this, &Panel::opacityChanged, preview_,
            [this](qreal value) { preview_->setPanelOpacity(qRound(value * 100)); });
    hoverTimer_ = new QTimer(this);
    hoverTimer_->setSingleShot(true);
    hoverTimer_->setInterval(180); // original Hopy hover-preview delay
    connect(hoverTimer_, &QTimer::timeout, this, [this] {
        if (behavior_.hoverPreview && hoverRow_ >= 0 && isVisible() && !spaceHeld_)
            showPreview(hoverRow_);
    });
    sideTimer_ = new QTimer(this);
    sideTimer_->setInterval(16);
    connect(sideTimer_, &QTimer::timeout, this, [this] {
        if (!preview_->isVisible()) {
            sideTimer_->stop();
            sideVelocity_ = sideCarry_ = 0.0;
            return;
        }
        sideCarry_ += sideVelocity_;
        const int step = static_cast<int>(sideCarry_);
        sideCarry_ -= step;
        if (step)
            preview_->scrollByPixels(step);
        sideVelocity_ *= sideFriction;
        if (qAbs(sideVelocity_) < sideStop) {
            sideVelocity_ = sideCarry_ = 0.0;
            sideTimer_->stop();
        }
    });
    connect(preview_, &Panel::dismissed, this, [this] { Q_EMIT previewVisibleChanged(false); });
    connect(model_, &QAbstractItemModel::modelAboutToBeReset, this, [this, delegate] {
        delegate->reset();
        hoverRow_ = -1;
        hoverTimer_->stop();
    });
    bodyLayout->addWidget(list_);
    help_ = new QWidget(body);
    help_->setObjectName(QStringLiteral("ClipboardHelp"));
    auto* helpLayout = new QVBoxLayout(help_);
    helpLayout->setContentsMargins(6, 0, 6, 0);
    auto* helpHeader = new QHBoxLayout;
    auto* helpBack = theme.button(Symbol::Back, tx(u"返回历史（Esc）"), help_);
    connect(helpBack, &QToolButton::clicked, this, &ClipboardPanel::closeHelp);
    helpHeader->addWidget(helpBack);
    helpHeader->addWidget(new QLabel(tx(u"剪贴板快捷键"), help_));
    helpHeader->addStretch();
    helpLayout->addLayout(helpHeader);
    helpText_ = new QPlainTextEdit(help_);
    helpText_->setObjectName(QStringLiteral("ClipboardHelpText"));
    helpText_->setReadOnly(true);
    helpText_->setFocusPolicy(Qt::NoFocus);
    helpText_->installEventFilter(this);
    helpLayout->addWidget(helpText_);
    bodyLayout->addWidget(help_);
    help_->hide();
    layoutColumn()->addWidget(body);
    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString& q) { Q_EMIT queryChanged(q, filter_); });
    // Shift+double-click confirms as plain text, mirroring Shift+Enter.
    connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.isValid())
            Q_EMIT pasteRequested(records_[index.row()],
                                  QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
        const auto index = list_->indexAt(pos);
        // With right-click bound to "open", the menu stays reachable by keyboard only.
        if (!index.isValid() || behavior_.openButton == Qt::RightButton)
            return;
        list_->setCurrentIndex(index);
        auto& menu = *new QMenu(this);
        connect(&menu, &QMenu::aboutToHide, &menu, &QObject::deleteLater);
        connect(this, &Panel::dismissed, &menu, &QMenu::close);
        menu.addAction(theme_.icon(Symbol::Copy), tx(u"回贴\tEnter"), this,
                       [this] { handleKey(Qt::Key_Return); });
        menu.addAction(theme_.icon(Symbol::Text), tx(u"纯文本回贴\tShift+Enter"), this,
                       [this] { handleKey(Qt::Key_Return, true); });
        menu.addAction(tx(u"预览\tSpace"), this,
                       [this] { showPreview(list_->currentIndex().row()); });
        if (behavior_.openKey || behavior_.openButton != Qt::NoButton)
            menu.addAction(theme_.icon(Symbol::Open),
                           tx(u"打开链接或文件\t") + openLabel(), this,
                           [this] { Q_EMIT openRequested(selected().id); });
        menu.addSeparator();
        menu.addAction(theme_.icon(Symbol::Star), tx(u"收藏 / 取消收藏"), this,
                       [this] { performAction(list_->currentIndex().row(), 0); });
        menu.addAction(theme_.icon(Symbol::PinToTop), tx(u"置顶 / 取消置顶"), this,
                       [this] { performAction(list_->currentIndex().row(), 1); });
        menu.addAction(theme_.icon(Symbol::Trash), tx(u"删除\tDelete"), this,
                       [this] { performAction(list_->currentIndex().row(), 2); });
        menu.popup(list_->viewport()->mapToGlobal(pos));
    });
    connect(&theme, &Theme::changed, list_->viewport(), qOverload<>(&QWidget::update));
    connect(this, &Panel::dismissed, this, [this] {
        // The Space release may never reach a hidden panel.
        spaceHeld_ = false;
        hidePreview();
        closeHelp();
        if (searching())
            closeSearch();
    });
}
void ClipboardPanel::selectNewestOnNextRecords() {
    selectNewest_ = true;
}
void ClipboardPanel::setRecords(ClipboardRecords records) {
    // A freshly opened panel starts at the newest record, as in the original;
    // later refreshes (flags, deletes, new captures) keep the user's place.
    const bool newest = std::exchange(selectNewest_, false);
    const bool restore = !searching_ && std::exchange(restoreBrowse_, false) && !newest;
    const auto id = newest ? QString{} : restore ? savedSelectionId_ : selected().id;
    const int previousRow = newest ? 0 : restore ? savedSelectionRow_ : list_->currentIndex().row();
    const auto scroll = newest ? 0 : restore ? savedScroll_ : list_->verticalScrollBar()->value();
    records_ = std::move(records);
    static_cast<CardDelegate*>(list_->itemDelegate())->thumbnails.retain(records_);
    static_cast<RecordsModel*>(model_)->replace(records_);
    int row = records_.isEmpty() ? -1
                                : std::clamp(previousRow, 0, static_cast<int>(records_.size()) - 1);
    bool found = false;
    for (int i = 0; i < records_.size(); ++i)
        if (records_[i].id == id) {
            row = i;
            found = true;
            break;
        }
    if (!records_.isEmpty())
        list_->setCurrentIndex(model_->index(row, 0));
    list_->verticalScrollBar()->setValue(scroll);
    if (!newest && !found && row >= 0)
        list_->scrollTo(model_->index(row, 0), QAbstractItemView::EnsureVisible);
    if (spaceHeld_ && !records_.isEmpty())
        showPreview(row);
    else if (!previewTargetId_.isEmpty() &&
             std::none_of(records_.cbegin(), records_.cend(), [this](const auto& r) {
                 return r.id == previewTargetId_;
             }))
        hidePreview();
}
ClipboardRecord ClipboardPanel::selected() const {
    const auto i = list_->currentIndex();
    return i.isValid() && i.row() < records_.size() ? records_[i.row()] : ClipboardRecord{};
}
void ClipboardPanel::setMaskSensitive(bool enabled) {
    masked_ = enabled;
    static_cast<CardDelegate*>(list_->itemDelegate())->masked = enabled;
    hidePreview();
    list_->viewport()->update();
}
void ClipboardPanel::setBehavior(const ClipboardBehavior& behavior) {
    behavior_ = behavior;
    if (!behavior.hoverPreview)
        hoverTimer_->stop();
}
void ClipboardPanel::sideScroll(int direction) {
    if (!preview_->isVisible())
        return;
    if (direction * sideVelocity_ < 0)
        sideVelocity_ = 0.0; // a reversal responds at once
    sideVelocity_ = qBound(-sideMaximum, sideVelocity_ + direction * sideKick, sideMaximum);
    if (!sideTimer_->isActive())
        sideTimer_->start();
}
QString ClipboardPanel::openLabel() const {
    QStringList keys;
    if (behavior_.openKey)
        keys << QKeySequence(behavior_.openKey).toString(QKeySequence::NativeText);
    if (behavior_.openButton == Qt::MiddleButton)
        keys << tx(u"中键");
    if (behavior_.openButton == Qt::RightButton)
        keys << tx(u"右键");
    return keys.join(QStringLiteral(" / "));
}
void ClipboardPanel::setPaused(bool paused) {
    paused_ = paused;
}
bool ClipboardPanel::searching() const {
    return searching_;
}
void ClipboardPanel::applySearchProgress(qreal t) {
    searchProgress_ = t;
    // Widths are driven directly so the row never reflows into a different
    // arrangement mid-animation; More/Close keep their place at the right edge.
    const int buttonWidth = searchButton_->sizeHint().width();
    const auto* row = toolbar_->layout();
    // The header always spans the fixed panel width, even before first layout.
    const int fullSearch = width() - row->contentsMargins().left() -
                           row->contentsMargins().right() - 2 * (buttonWidth + row->spacing());
    // Mid-transition both sides stay visible; visibility settles at rest.
    const bool moving = searchAnimation_->state() == QAbstractAnimation::Running;
    browse_->setVisible(moving || t < 1.0);
    searchButton_->setVisible(moving || t < 1.0);
    searchBox_->setVisible(moving || t > 0.0);
    browse_->setFixedWidth(qRound(browseWidth_ * (1.0 - t)));
    searchButton_->setFixedWidth(qRound(buttonWidth * (1.0 - t)));
    searchBox_->setFixedWidth(qMax(0, qRound(fullSearch * t)));
    // Cross-fade: the old controls are gone before the field is fully opaque.
    browseFade_->setOpacity(qBound(0.0, 1.0 - t * 1.6, 1.0));
    searchFade_->setOpacity(qBound(0.0, (t - 0.2) / 0.8, 1.0));
    // Fully opaque controls need no offscreen effect pass (it also misplaces
    // them in QWidget::render, which the evidence capture relies on).
    browseFade_->setEnabled(browseFade_->opacity() < 1.0);
    searchFade_->setEnabled(searchFade_->opacity() < 1.0);
}
void ClipboardPanel::animateSearchTo(qreal target) {
    searchAnimation_->stop();
    if (!animateSearch_ || !isVisible()) {
        applySearchProgress(target);
        return;
    }
    // Both sides are visible for the whole transition: the field can take focus
    // at once and the filters are back the moment search closes.
    browse_->show();
    searchButton_->show();
    searchBox_->show();
    // Reversals continue from the current frame with a proportional duration.
    const qreal distance = qAbs(target - searchProgress_);
    searchAnimation_->setDuration(qMax(1, qRound(searchMotionMs * distance)));
    searchAnimation_->setStartValue(searchProgress_);
    searchAnimation_->setEndValue(target);
    searchAnimation_->start();
}
void ClipboardPanel::setFilter(int filter) {
    filter_ = filter;
    for (int i = 0; i < filters_.size(); ++i)
        filters_[i]->setChecked(filter == i);
    Q_EMIT queryChanged(search_->text(), filter_);
}
void ClipboardPanel::showSearch() {
    closeHelp();
    if (searching())
        return;
    if (!restoreBrowse_) {
        savedScroll_ = list_->verticalScrollBar()->value();
        savedSelectionId_ = selected().id;
        savedSelectionRow_ = list_->currentIndex().row();
    }
    restoreBrowse_ = false;
    Q_EMIT searchModeChanged(true);
    // QWindow updates the native focus policy in place; QWidget::setWindowFlag hides it.
    (void)winId();
    windowHandle()->setFlag(Qt::WindowDoesNotAcceptFocus, false);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    refreshFrame(); // the flag change rebuilt the native style
    update();
    searching_ = true;
    animateSearchTo(1.0);
    if (!isVisible())
        show();
    windowHandle()->requestActivate();
    search_->setFocus();
}
void ClipboardPanel::closeSearch() {
    if (!searching())
        return;
    searching_ = false;
    animateSearchTo(0.0);
    search_->clearFocus();
    restoreBrowse_ = true;
    {
        const QSignalBlocker blocked(search_);
        search_->clear();
    }
    Q_EMIT queryChanged({}, filter_);
    if (windowHandle())
        windowHandle()->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    refreshFrame(); // the flag change rebuilt the native style
    // Changing the native style can drop the window surface; only dirty regions
    // would be flushed again, leaving the untouched list blank.
    update();
    Q_EMIT searchModeChanged(false);
}
void ClipboardPanel::performAction(int row, int action) {
    if (!help_->isHidden() || row < 0 || row >= records_.size())
        return;
    const auto r = records_[row];
    if (action == 0)
        Q_EMIT toggleFlagRequested(r.id, ClipFlag::Favorite);
    if (action == 1)
        Q_EMIT toggleFlagRequested(r.id, ClipFlag::Pinned);
    if (action == 2)
        Q_EMIT removeRequested(r.id);
}
void ClipboardPanel::showPreview(int row) {
    if (row < 0 || row >= records_.size())
        return;
    if (previewTargetId_ == records_[row].id)
        return;
    previewTargetId_ = records_[row].id;
    ++previewToken_;
    // Returning to the already displayed row still invalidates a pending
    // request for another row, but does not need to load this payload again.
    if (preview_->isVisible() && preview_->recordId() == previewTargetId_)
        return;
    Q_EMIT previewRequested(previewTargetId_, previewToken_);
}
void ClipboardPanel::hidePreview() {
    ++previewToken_;
    previewTargetId_.clear();
    hoverTimer_->stop();
    sideTimer_->stop();
    sideVelocity_ = sideCarry_ = 0.0;
    preview_->hide();
}
void ClipboardPanel::hoverAt(QPoint pos) {
    const auto index = list_->indexAt(pos);
    const int row = index.isValid() ? index.row() : -1;
    const int action =
        index.isValid() ? ClipCardLayout(list_->visualRect(index)).actionAt(pos) : -1;
    static_cast<CardDelegate*>(list_->itemDelegate())->setHover(row, action);
    if (row == hoverRow_)
        return;
    hoverRow_ = row;
    if (row < 0)
        return;
    // Follow the pointer once a preview is open; otherwise wait briefly.
    if (!behavior_.hoverPreview)
        return;
    if (preview_->isVisible())
        showPreview(row);
    else
        hoverTimer_->start();
}
void ClipboardPanel::displayPreview(const ClipboardRecord& r, quint64 token) {
    if (token != previewToken_ || r.id != previewTargetId_ || !isVisible())
        return;
    const bool wasVisible = preview_->isVisible();
    preview_->present(r, masked_, geometry(), behavior_.previewLeft, openLabel());
    if (!wasVisible)
        Q_EMIT previewVisibleChanged(true);
}
void ClipboardPanel::showHelp() {
    closeSearch();
    spaceHeld_ = false;
    hidePreview();
    hoverRow_ = -1;
    static_cast<CardDelegate*>(list_->itemDelegate())->reset();
    QStringList lines{tx(u"↑ / ↓、Home / End：选择"), tx(u"Enter：确认；Shift+Enter：纯文本确认"),
                      tx(u"1–5：确认对应记录"), tx(u"← / →、Tab：切换过滤；再次点击图标取消过滤"),
                      tx(u"/、Ctrl+F：搜索；Esc：退出搜索 / 隐藏"),
                      tx(u"F：收藏；T：置顶；D / Delete：删除")};
    if (behavior_.spacePreview)
        lines << tx(u"按住 Space：预览；Ctrl+滚轮 / 鼠标侧键：预览翻页");
    if (!openLabel().isEmpty())
        lines << tx(u"%1：打开链接、邮箱或定位文件").arg(openLabel());
    lines << tx(u"S：设置；H：快捷键说明");
    helpText_->setPlainText(lines.join(QStringLiteral("\n\n")));
    list_->hide();
    help_->show();
}
void ClipboardPanel::closeHelp() {
    help_->hide();
    list_->show();
}
void ClipboardPanel::handleKey(int key, bool shift, bool control, bool down) {
    if (!help_->isHidden()) {
        if (down && (key == Qt::Key_Escape || key == Qt::Key_H || key == Qt::Key_Return))
            closeHelp();
        return;
    }
    if (!down) {
        if (key == Qt::Key_Space) {
            spaceHeld_ = false;
            hidePreview();
        }
        return;
    }
    if (key == Qt::Key_Escape) {
        if (spaceHeld_ || preview_->isVisible() || !previewTargetId_.isEmpty())
            hidePreview();
        else if (searching())
            closeSearch();
        else
            hide();
        return;
    }
    if (key == Qt::Key_Slash || (control && key == Qt::Key_F)) {
        showSearch();
        return;
    }
    int row = list_->currentIndex().row();
    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End) {
        if (records_.isEmpty())
            return;
        if (key == Qt::Key_Home)
            row = 0;
        else if (key == Qt::Key_End)
            row = static_cast<int>(records_.size() - 1);
        else
            row = qBound(0, row + (key == Qt::Key_Up ? -1 : 1),
                         static_cast<int>(records_.size() - 1));
        list_->setCurrentIndex(model_->index(row, 0));
        list_->scrollTo(model_->index(row, 0));
        if (spaceHeld_ || preview_->isVisible() || !previewTargetId_.isEmpty())
            showPreview(row);
        return;
    }
    if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Tab) {
        int direction = key == Qt::Key_Left || (key == Qt::Key_Tab && shift) ? -1 : 1;
        setFilter((filter_ + 1 + direction + 5) % 5 - 1);
        return;
    }
    if (key >= Qt::Key_1 && key <= Qt::Key_5) {
        const int index = key - Qt::Key_1;
        if (index < records_.size())
            Q_EMIT pasteRequested(records_[index], shift);
        return;
    }
    if (key == Qt::Key_Return && !selected().id.isEmpty())
        Q_EMIT pasteRequested(selected(), shift);
    if (key == Qt::Key_F || (control && key == Qt::Key_D))
        performAction(row, 0);
    if (key == Qt::Key_T || (control && key == Qt::Key_P))
        performAction(row, 1);
    if (key == Qt::Key_Delete || (key == Qt::Key_D && !control))
        performAction(row, 2);
    // Key repeat while Space is held must not re-request (and restart) the preview.
    if (key == Qt::Key_Space && behavior_.spacePreview && !std::exchange(spaceHeld_, true))
        showPreview(row);
    if (key == Qt::Key_S)
        Q_EMIT settingsRequested();
    if (key == Qt::Key_H)
        showHelp();
    if (!control && behavior_.openKey && key == behavior_.openKey && !selected().id.isEmpty() &&
        !(masked_ && selected().sensitive))
        Q_EMIT openRequested(selected().id);
}
void ClipboardPanel::keyPressEvent(QKeyEvent* e) {
    handleKey(e->key(), e->modifiers() & Qt::ShiftModifier, e->modifiers() & Qt::ControlModifier);
    e->accept();
}
void ClipboardPanel::keyReleaseEvent(QKeyEvent* e) {
    // Windows auto-repeat arrives as release/press pairs; only a real release counts.
    if (!e->isAutoRepeat())
        handleKey(e->key(), false, false, false);
    e->accept();
}
bool ClipboardPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == listViewport_) {
        if (event->type() == QEvent::MouseMove)
            hoverAt(static_cast<QMouseEvent*>(event)->position().toPoint());
        if (event->type() == QEvent::Leave) {
            hoverRow_ = -1;
            static_cast<CardDelegate*>(list_->itemDelegate())->setHover(-1, -1);
            if (!spaceHeld_)
                hidePreview();
        }
        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if ((wheel->modifiers() & Qt::ControlModifier) && preview_->isVisible()) {
                preview_->scrollByPixels(-wheel->angleDelta().y());
                return true;
            }
            // Content moves under a still pointer; refresh hover after scrolling.
            QTimer::singleShot(0, this, [this] {
                if (listViewport_)
                    hoverAt(listViewport_->mapFromGlobal(QCursor::pos()));
            });
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && watched == listViewport_ &&
        behavior_.openButton != Qt::NoButton &&
        static_cast<QMouseEvent*>(event)->button() == behavior_.openButton) {
        const auto index = list_->indexAt(static_cast<QMouseEvent*>(event)->position().toPoint());
        if (index.isValid() && !(masked_ && records_[index.row()].sensitive)) {
            list_->setCurrentIndex(index);
            Q_EMIT openRequested(records_[index.row()].id);
            return true;
        }
    }
    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick) &&
        watched == listViewport_) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const auto index = list_->indexAt(mouse->position().toPoint());
            const int action =
                index.isValid()
                    ? ClipCardLayout(list_->visualRect(index)).actionAt(mouse->position().toPoint())
                    : -1;
            if (event->type() == QEvent::MouseButtonPress) {
                pressedAction_ = action;
                pressedActionId_ = action >= 0 ? records_[index.row()].id : QString{};
                if (action >= 0)
                    list_->setCurrentIndex(index);
            } else if (event->type() == QEvent::MouseButtonRelease) {
                const auto previous = std::exchange(pressedAction_, -1);
                const auto id = std::exchange(pressedActionId_, QString{});
                if (action >= 0 && action == previous && records_[index.row()].id == id)
                    performAction(index.row(), action);
                if (previous >= 0)
                    return true;
            }
            if (action >= 0)
                return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (watched == search_ && key->key() != Qt::Key_Escape && key->key() != Qt::Key_Return &&
            key->key() != Qt::Key_Down && key->key() != Qt::Key_Up)
            return false;
        handleKey(key->key(), key->modifiers() & Qt::ShiftModifier,
                  key->modifiers() & Qt::ControlModifier);
        return true;
    }
    if (event->type() == QEvent::KeyRelease &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space) {
        if (!static_cast<QKeyEvent*>(event)->isAutoRepeat()) {
            spaceHeld_ = false;
            hidePreview();
        }
        return true;
    }
    return Panel::eventFilter(watched, event);
}
} // namespace mt
