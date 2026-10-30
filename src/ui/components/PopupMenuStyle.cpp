#include "PopupMenuStyle.h"
#include "Theme.h"
#include "platform/windows/MenuFrame.h"
#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QTimer>

namespace mt {
PopupMenuStyle::PopupMenuStyle(Theme& theme)
    : QObject(&theme), theme_(theme), frame_(std::make_unique<MenuFrame>()) {
    qApp->installEventFilter(this);
    connect(&theme_, &Theme::changed, this, [this] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && (qobject_cast<QMenu*>(widget) ||
                                       widget->property("settingsComboPopup").toBool()))
                applyFrame(*widget);
    });
}
PopupMenuStyle::~PopupMenuStyle() = default;
void PopupMenuStyle::applyFrame(QWidget& popup) {
    // Do not create a fresh HWND after a popup has already been dismissed.
    if (popup.isVisible() && popup.internalWinId() && popup.windowHandle())
        popup.setProperty("systemMenuFrame", frame_->apply(*popup.windowHandle(),
                                                          theme_.name() == QStringLiteral("dark")));
}
bool PopupMenuStyle::eventFilter(QObject* watched, QEvent* event) {
    auto* menu = qobject_cast<QMenu*>(watched);
    auto* popup = qobject_cast<QWidget*>(watched);
    if (!popup || (!menu && !popup->property("settingsComboPopup").toBool()))
        return false;
    if (event->type() == QEvent::Polish) {
        popup->setWindowFlag(Qt::FramelessWindowHint);
        popup->setWindowFlag(Qt::NoDropShadowWindowHint, false);
        popup->setAttribute(Qt::WA_TranslucentBackground, false);
        if (menu) {
            connect(menu, &QMenu::aboutToShow, this, [menu] {
                // Keep the reference's compact text layout without changing QAction
                // icons used by other views, shortcuts, or callbacks.
                for (auto* action : menu->actions())
                    action->setIconVisibleInMenu(false);
            });
        }
    } else if (event->type() == QEvent::Show) {
        applyFrame(*popup);
        QTimer::singleShot(0, this, [this, popup = QPointer<QWidget>(popup)] {
            if (popup)
                applyFrame(*popup);
        });
    }
    return false;
}
} // namespace mt
