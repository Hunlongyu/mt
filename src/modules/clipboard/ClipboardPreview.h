#pragma once
#include "ClipboardRecord.h"
#include "ui/components/Panel.h"
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QAbstractScrollArea;
namespace mt {
class Theme;
// Non-activating full-content preview docked beside the clipboard panel. It
// shares Panel's rounded surface, shadow and opacity; height never exceeds the
// panel, overflow scrolls (wheel over the list with Ctrl, or the preview itself).
class ClipboardPreview final : public Panel {
    Q_OBJECT
  public:
    explicit ClipboardPreview(Theme& theme, QWidget* owner);
    // openKeys names the active open bindings ("O / 中键"); empty hides the hint.
    void present(const ClipboardRecord& record, bool masked, QRect anchor, bool preferLeft = true,
                 const QString& openKeys = {});
    void scrollByPixels(int dy);
    QString recordId() const {
        return recordId_;
    }

  private:
    void updatePercent();
    QLabel* info_;
    QLabel* percent_;
    QPlainTextEdit* text_;
    QScrollArea* imageArea_;
    QLabel* image_;
    QAbstractScrollArea* active_ = nullptr;
    QString recordId_;
    int lineRemainder_ = 0;
};
} // namespace mt
