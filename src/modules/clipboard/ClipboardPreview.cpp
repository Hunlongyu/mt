#include "ClipboardPreview.h"
#include "ClipboardText.h"
#include "infrastructure/i18n/I18n.h"
#include "ui/components/Theme.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

namespace mt {
namespace {
constexpr int padding = 12;
constexpr int gap = 8;
constexpr int maxContentWidth = 360;
QString openVerb(const ClipboardRecord& record) {
    if (record.kind == ClipKind::Files)
        return tx(u"在资源管理器中定位");
    if (record.kind == ClipKind::Image)
        return {};
    switch (openTarget(record.text).kind) {
    case OpenKind::Url:
        return tx(u"打开链接");
    case OpenKind::Email:
        return tx(u"打开邮箱");
    case OpenKind::Path:
        return tx(u"在资源管理器中定位");
    case OpenKind::None:
        break;
    }
    return {};
}
} // namespace
ClipboardPreview::ClipboardPreview(Theme& theme, QWidget* owner)
    : Panel(theme, tx(u"MT · 剪贴板预览"), owner) {
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    auto* column = layoutColumn();
    column->setContentsMargins(padding, padding - 2, padding, padding);
    column->setSpacing(6);
    auto* infoRow = new QHBoxLayout;
    infoRow->setSpacing(8);
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("PreviewInfo"));
    percent_ = new QLabel(this);
    percent_->setObjectName(QStringLiteral("PreviewInfo"));
    percent_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (auto* label : {info_, percent_}) {
        auto font = label->font();
        font.setPixelSize(11);
        label->setFont(font);
    }
    infoRow->addWidget(info_, 1);
    infoRow->addWidget(percent_);
    column->addLayout(infoRow);

    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("PreviewText"));
    text_->setAccessibleName(tx(u"记录完整内容"));
    text_->setReadOnly(true);
    text_->setFrameShape(QFrame::NoFrame);
    text_->setFocusPolicy(Qt::NoFocus);
    text_->setTextInteractionFlags(Qt::NoTextInteraction);
    text_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    text_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    text_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    text_->document()->setDocumentMargin(0);
    auto font = text_->font();
    font.setPixelSize(14);
    text_->setFont(font);
    column->addWidget(text_, 1);

    imageArea_ = new QScrollArea(this);
    imageArea_->setObjectName(QStringLiteral("PreviewImage"));
    imageArea_->setFrameShape(QFrame::NoFrame);
    imageArea_->setFocusPolicy(Qt::NoFocus);
    imageArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    imageArea_->viewport()->setAutoFillBackground(false);
    image_ = new QLabel(imageArea_);
    image_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    imageArea_->setWidget(image_);
    column->addWidget(imageArea_, 1);
    imageArea_->hide();

    for (QAbstractScrollArea* area : {static_cast<QAbstractScrollArea*>(text_),
                                      static_cast<QAbstractScrollArea*>(imageArea_)}) {
        connect(area->verticalScrollBar(), &QScrollBar::valueChanged, this,
                &ClipboardPreview::updatePercent);
        connect(area->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                &ClipboardPreview::updatePercent);
    }
}
void ClipboardPreview::present(const ClipboardRecord& record, bool masked, QRect anchor,
                               bool preferLeft, const QString& openKeys) {
    recordId_ = record.id;
    lineRemainder_ = 0;
    auto* screen = QGuiApplication::screenAt(anchor.center());
    const auto available = screen ? screen->availableGeometry() : anchor.adjusted(-800, 0, 800, 0);
    const int maxHeight = std::max(64, std::min(anchor.height(), available.bottom() - anchor.top()));
    const int contentWidth = std::min(maxContentWidth, std::max(200, anchor.width() - 2 * padding));
    const bool hidden = masked && record.sensitive;

    QStringList parts;
    parts << (record.kind == ClipKind::Image     ? tx(u"图片")
              : record.kind == ClipKind::Files   ? tx(u"文件")
              : record.kind == ClipKind::Html    ? tx(u"富文本")
              : record.kind == ClipKind::Formats ? tx(u"应用专用内容")
                                                 : tx(u"文本"));
    const auto body = hidden ? tx(u"••••••••  敏感内容已遮蔽") : record.summary();
    if (!hidden && record.kind != ClipKind::Image && record.kind != ClipKind::Formats) {
        parts << tx(u"%1 字符").arg(clipCharacters(record.summary()));
        parts << tx(u"%1 行").arg(clipLines(record.summary()));
    }
    parts << record.captured.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (const auto verb = openVerb(record); !hidden && !verb.isEmpty() && !openKeys.isEmpty())
        parts << openKeys + u' ' + verb;
    info_->setText(parts.join(QStringLiteral("  ·  ")));
    const int infoHeight = info_->sizeHint().height() + layoutColumn()->spacing();

    int width = contentWidth;
    int contentHeight = 0;
    if (record.kind == ClipKind::Image && !hidden) {
        auto pixmap = QPixmap::fromImage(QImage::fromData(record.image));
        if (pixmap.isNull()) {
            image_->setText(tx(u"（图片不可用）"));
            image_->adjustSize();
        } else {
            // Keep native size; only reduce width so tall images scroll sharp.
            const auto scale = devicePixelRatioF();
            pixmap.setDevicePixelRatio(scale);
            const int logicalWidth = qRound(pixmap.width() / scale);
            if (logicalWidth > contentWidth) {
                pixmap = pixmap.scaledToWidth(qRound(contentWidth * scale), Qt::SmoothTransformation);
                pixmap.setDevicePixelRatio(scale);
            }
            image_->setPixmap(pixmap);
            image_->setFixedSize(pixmap.deviceIndependentSize().toSize());
            width = std::max(160, image_->width());
        }
        contentHeight = image_->height();
        text_->hide();
        imageArea_->show();
        active_ = imageArea_;
    } else {
        text_->setPlainText(body);
        // Measure ordinary content exactly; very large pastes simply fill the cap.
        if (body.size() > 8192) {
            contentHeight = maxHeight;
        } else {
            QTextDocument measure;
            measure.setDefaultFont(text_->font());
            measure.setDocumentMargin(0);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            measure.setDefaultTextOption(option);
            measure.setPlainText(body);
            measure.setTextWidth(contentWidth);
            contentHeight = qCeil(measure.size().height()) + 4;
        }
        imageArea_->hide();
        text_->show();
        active_ = text_;
    }
    const int w = width + 2 * padding;
    const int h = std::min(maxHeight, contentHeight + infoHeight + 2 * padding - 2);
    setFixedSize(w, std::max(h, infoHeight + 2 * padding + 20));
    active_->verticalScrollBar()->setValue(0);
    updatePercent();

    // Use the preferred side; flip instead of covering the panel when it does not fit.
    const int left = anchor.left() - gap - width - 2 * padding;
    const int right = anchor.right() + 1 + gap;
    const bool leftFits = left >= available.left();
    const bool rightFits = right + w <= available.right() + 1;
    const bool useLeft = preferLeft ? leftFits || !rightFits : !rightFits && leftFits;
    const int x = std::clamp(useLeft ? left : right, available.left(),
                             std::max(available.left(), available.right() + 1 - w));
    const int y = std::clamp(anchor.top(), available.top(),
                             std::max(available.top(), available.bottom() + 1 - height()));
    move(x, y);
    if (!isVisible())
        show();
    raise();
}
void ClipboardPreview::scrollByPixels(int dy) {
    if (!active_ || !isVisible())
        return;
    auto* bar = active_->verticalScrollBar();
    if (active_ == text_) {
        // QPlainTextEdit scrolls by lines; carry sub-line motion between wheel events.
        lineRemainder_ += dy;
        const int lineHeight = std::max(1, text_->fontMetrics().lineSpacing());
        const int lines = lineRemainder_ / lineHeight;
        lineRemainder_ -= lines * lineHeight;
        bar->setValue(bar->value() + lines);
        return;
    }
    bar->setValue(bar->value() + dy);
}
void ClipboardPreview::updatePercent() {
    auto* bar = active_ ? active_->verticalScrollBar() : nullptr;
    if (!bar || bar->maximum() <= 0) {
        percent_->clear();
        return;
    }
    percent_->setText(QStringLiteral("%1%").arg(qRound(100.0 * bar->value() / bar->maximum())));
}
} // namespace mt
