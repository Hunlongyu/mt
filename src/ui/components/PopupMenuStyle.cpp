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
            if (auto* menu = qobject_cast<QMenu*>(widget); menu && menu->isVisible())
                applyFrame(*menu);
    });
}
PopupMenuStyle::~PopupMenuStyle() = default;
void PopupMenuStyle::applyFrame(QMenu& menu) {
    // Do not create a fresh HWND after a popup has already been dismissed.
    if (menu.isVisible() && menu.internalWinId() && menu.windowHandle())
        menu.setProperty("systemMenuFrame", frame_->apply(*menu.windowHandle(),
                                                          theme_.name() == QStringLiteral("dark")));
}
bool PopupMenuStyle::eventFilter(QObject* watched, QEvent* event) {
    auto* menu = qobject_cast<QMenu*>(watched);
    if (!menu)
        return false;
    if (event->type() == QEvent::Polish) {
        menu->setWindowFlag(Qt::FramelessWindowHint);
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, false);
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        connect(menu, &QMenu::aboutToShow, this, [menu] {
            // Keep the reference's compact text layout without changing QAction
            // icons used by other views, shortcuts, or callbacks.
            for (auto* action : menu->actions())
                action->setIconVisibleInMenu(false);
        });
    } else if (event->type() == QEvent::Show) {
        applyFrame(*menu);
        QTimer::singleShot(0, this, [this, menu = QPointer<QMenu>(menu)] {
            if (menu)
                applyFrame(*menu);
        });
    }
    return false;
}
} // namespace mt
