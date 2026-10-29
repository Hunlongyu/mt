#pragma once
#include "ui/components/Theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <functional>
class QLabel;
class QScrollArea;
class QPlainTextEdit;
class QPushButton;
class QMessageBox;

namespace mt::settingsui {
// Small reusable Qt controls; layouts, focus and input remain native Widgets.
class ComboBox final : public QComboBox {
  public:
    explicit ComboBox(Theme& theme, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Theme& theme_;
};
class SpinBox final : public QSpinBox {
  public:
    explicit SpinBox(Theme& theme, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Theme& theme_;
};
class Switch final : public QCheckBox {
  public:
    Switch(Theme& theme, const QString& label, QWidget* parent = nullptr);
    QSize sizeHint() const override {
        return {42, 26};
    }

  protected:
    void paintEvent(QPaintEvent*) override;
    bool hitButton(const QPoint& p) const override {
        return rect().contains(p);
    }

  private:
    Theme& theme_;
};
class ThemePreview final : public QWidget {
  public:
    ThemePreview(Theme& theme, QString mode, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    Theme& theme_;
    QString mode_;
};
class GridPreview final : public QWidget {
  public:
    explicit GridPreview(Theme& theme, QWidget* parent = nullptr);
    void setGrid(int columns, int rows);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    Theme& theme_;
    int columns_ = 4, rows_ = 7;
};
QLabel* label(const QString& text, const char* role = nullptr);
QVBoxLayout* content(QScrollArea*& scroll, QWidget* parent);
QVBoxLayout* card(QVBoxLayout* layout, const QString& title = {});
QVBoxLayout* fields(QVBoxLayout* card);
QPlainTextEdit* textArea(QVBoxLayout* fields, const QString& title, const QString& value,
                         const QString& placeholder, const QString& objectName);
void row(QVBoxLayout* card, const QString& title, const QString& description, QWidget* control);
void note(QVBoxLayout* layout, const QString& text);
void iconButton(QPushButton* button, Theme& theme, Symbol symbol);
QStringList processes(const QString& text);
QMessageBox* confirm(QWidget* parent, const QString& title, const QString& text,
                     const QString& acceptLabel, std::function<void()> accepted,
                     const QString& objectName = {});
QString styleSheet();
} // namespace mt::settingsui
