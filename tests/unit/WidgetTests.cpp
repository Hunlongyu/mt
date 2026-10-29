#include <dwmapi.h>
#include <windows.h>

#include "modules/clipboard/ClipboardPanel.h"
#include "modules/launcher/LauncherPanel.h"
#include "ui/components/Placement.h"
#include "ui/components/Theme.h"
#include "ui/shell/SettingsDialog.h"
#include "platform/windows/LockPassword.h"
#include "platform/windows/DesktopServices.h"
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWindow>
#include <QtPlugin>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QtTest>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
using namespace mt;
class WidgetTests final : public QObject {
    Q_OBJECT
  private:
    static QToolButton* button(QWidget& root, const QString& label) {
        for (auto* b : root.findChildren<QToolButton*>())
            if (b->accessibleName() == label)
                return b;
        return nullptr;
    }
    static QImage renderSurface(QWidget& widget) {
        QImage image(widget.size() * widget.devicePixelRatioF(),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(widget.devicePixelRatioF());
        image.fill(Qt::transparent);
        widget.render(&image);
        return image;
    }
    static bool nativeCornersAvailable() {
        QWidget probe;
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
        return SUCCEEDED(DwmGetWindowAttribute(reinterpret_cast<HWND>(probe.winId()),
                                               DWMWA_WINDOW_CORNER_PREFERENCE,
                                               &corner, sizeof(corner)));
    }
    static bool systemMenuFrame(QMenu& menu) {
        const auto hwnd = reinterpret_cast<HWND>(menu.internalWinId());
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
        RECT client{}, window{};
        DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        GetClientRect(hwnd, &client);
        GetWindowRect(hwnd, &window);
        const bool valid = hwnd && menu.property("systemMenuFrame").toBool() &&
                           !menu.testAttribute(Qt::WA_TranslucentBackground) &&
                           !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) &&
                           (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) &&
                           SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                                           &corner, sizeof(corner))) &&
                           corner == DWMWCP_ROUNDSMALL && GetClientRect(hwnd, &client) &&
                           GetWindowRect(hwnd, &window) &&
                           client.right - client.left == window.right - window.left &&
                           client.bottom - client.top == window.bottom - window.top;
        if (!valid)
            qWarning() << "Menu frame:" << hwnd << "visible" << menu.isVisible()
                       << IsWindow(hwnd) << menu.property("systemMenuFrame") << "translucent"
                       << menu.testAttribute(Qt::WA_TranslucentBackground) << "styles" << Qt::hex
                       << GetWindowLongPtrW(hwnd, GWL_STYLE) << GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
                       << Qt::dec << "corner" << corner << "client" << client.right - client.left
                       << client.bottom - client.top << "window" << window.right - window.left
                       << window.bottom - window.top;
        return valid;
    }
    // DWM-framed panel: opaque surface, standard rounded corners, client area equal
    // to the window (no visible caption or border inset), no hard window region.
    static bool systemPanelFrame(QWidget& panel) {
        const auto hwnd = reinterpret_cast<HWND>(panel.winId());
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
        RECT client{}, window{};
        const auto region = CreateRectRgn(0, 0, 0, 0);
        const bool noRegion = region && GetWindowRgn(hwnd, region) == ERROR;
        if (region)
            DeleteObject(region);
        const bool valid =
            noRegion && !panel.testAttribute(Qt::WA_TranslucentBackground) &&
            !panel.windowHandle()->format().hasAlpha() &&
            (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) &&
            SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                                            sizeof(corner))) &&
            corner == DWMWCP_ROUND && GetClientRect(hwnd, &client) && GetWindowRect(hwnd, &window) &&
            client.right - client.left == window.right - window.left &&
            client.bottom - client.top == window.bottom - window.top;
        if (!valid)
            qWarning() << panel.windowTitle() << "frame: region" << !noRegion << "style" << Qt::hex
                       << GetWindowLongPtrW(hwnd, GWL_STYLE) << Qt::dec << "corner" << corner
                       << "client" << client.right - client.left << "window"
                       << window.right - window.left;
        return valid;
    }
  private Q_SLOTS:
    void panelsUseTheSystemShadowAndCorners() {
        if (!nativeCornersAvailable())
            QSKIP("Windows 11 DWM corner API is unavailable on this host.");
        Theme theme;
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        LauncherPanel launcher(theme, service);
        ClipboardPanel clipboard(theme);
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            for (auto* panel : {static_cast<Panel*>(&launcher), static_cast<Panel*>(&clipboard)}) {
                const auto contentSize = panel->size();
                panel->setPanelOpacity(95);
                panel->showNear({80, 80}, false);
                QApplication::processEvents();
                QVERIFY(systemPanelFrame(*panel));
                QCOMPARE(panel->size(), contentSize);
                // No separate decoration window any more.
                QVERIFY(!panel->findChild<QWidget*>(QStringLiteral("PanelShadow")));
                // Uniform opacity keeps the frame; only per-pixel alpha loses it.
                panel->setPanelOpacity(70);
                QVERIFY(systemPanelFrame(*panel));
                panel->hide();
            }
            // Switching focus policy rebuilds the native style; the frame must follow.
            clipboard.showNear({80, 80}, false);
            clipboard.showSearch();
            QVERIFY(systemPanelFrame(clipboard));
            clipboard.closeSearch();
            QVERIFY(systemPanelFrame(clipboard));
            clipboard.hide();
            launcher.showNear({80, 80});
            QTest::keyClick(&launcher, Qt::Key_F, Qt::ControlModifier);
            auto* search = launcher.findChild<Panel*>();
            QVERIFY(search && search->isVisible());
            QVERIFY(systemPanelFrame(*search));
            search->close();
            launcher.hide();
        }
    }
    void menusKeepKeyboardActionsAndUseSystemFrames() {
        const bool corners = nativeCornersAvailable();
        Theme theme;
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            QMenu menu;
            auto* first = menu.addAction(theme.icon(Symbol::Copy), QStringLiteral("回贴\tEnter"));
            auto* disabled = menu.addAction(QStringLiteral("不可用项"));
            disabled->setEnabled(false);
            QSignalSpy disabledTriggered(disabled, &QAction::triggered);
            menu.addSeparator();
            auto* favorite = menu.addAction(QStringLiteral("收藏"));
            favorite->setCheckable(true);
            QSignalSpy triggered(favorite, &QAction::triggered);
            menu.popup({30, 30});
            QApplication::processEvents();
            QVERIFY(QTest::qWaitFor([&] { return menu.windowOpacity() == 1; }));
            if (corners) QVERIFY(systemMenuFrame(menu));
            QCOMPARE(menu.pos(), QPoint(30, 30));
            QVERIFY(menu.mask().isEmpty());
            QCOMPARE(menu.actionGeometry(first).height(), 30);
            // DEC-025: items sit inside a 4px inset so the rounded highlight never
            // touches the DWM frame.
            QVERIFY(menu.actionGeometry(first).top() >= 4);
            QVERIFY(menu.actionGeometry(first).left() >= 4);
            QVERIFY(menu.actionGeometry(first).right() <= menu.width() - 5);
            QVERIFY(!first->isIconVisibleInMenu());
            QVERIFY(!first->icon().isNull());
            menu.setActiveAction(first);
            const auto image = renderSurface(menu);
            const auto geometry = menu.actionGeometry(first);
            const auto scale = image.devicePixelRatio();
            QCOMPARE(image.pixelColor((geometry.topLeft() + QPoint(6, 15)) * scale),
                     theme.color(QStringLiteral("surface-hover")));
            // Rounded corner: the very corner pixel keeps the menu surface.
            QVERIFY(image.pixelColor(geometry.topLeft() * scale) !=
                    theme.color(QStringLiteral("surface-hover")));
            QCOMPARE(image.pixelColor(QPoint(2, geometry.center().y()) * scale),
                     theme.color(QStringLiteral("surface-raised")));
            QTest::keyClick(&menu, Qt::Key_Down);
            // Windows styles may let the selection visit disabled actions. They
            // must remain inert, and navigation must continue to the next item.
            if (menu.activeAction() == disabled) {
                QTest::keyClick(&menu, Qt::Key_Return);
                QCOMPARE(disabledTriggered.size(), 0);
                QVERIFY(menu.isVisible());
                QTest::keyClick(&menu, Qt::Key_Down);
            }
            QCOMPARE(menu.activeAction(), favorite);
            QTest::keyClick(&menu, Qt::Key_Return);
            QCOMPARE(triggered.size(), 1);
            QVERIFY(favorite->isChecked());
            QVERIFY(!menu.isVisible());
            // Qt drops a popup requested while the triggered menu is still closing;
            // a person can never reopen it that fast, so let the close settle.
            QTest::qWait(400);
            menu.popup({30, 30});
            QApplication::processEvents();
            QVERIFY(QTest::qWaitFor([&] { return menu.isVisible() && menu.windowOpacity() == 1; }));
            if (corners) QVERIFY(systemMenuFrame(menu));
            QTest::keyClick(&menu, Qt::Key_Escape);
            QVERIFY(!menu.isVisible());
            QCOMPARE(triggered.size(), 1);

            QLineEdit editor;
            const auto editorMenu = std::unique_ptr<QMenu>(editor.createStandardContextMenu());
            editorMenu->ensurePolished();
            editorMenu->popup({30, 30});
            QApplication::processEvents();
            QVERIFY(QTest::qWaitFor([&] { return editorMenu->windowOpacity() == 1; }));
            if (corners) QVERIFY(systemMenuFrame(*editorMenu));
            editorMenu->hide();
        }
    }
    void contextAndToolbarMenusUseTheSameSystemFrame() {
        const bool corners = nativeCornersAvailable();
        Theme theme;
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        LauncherPanel launcher(theme, service);
        ClipboardPanel clipboard(theme);
        ClipboardRecord record;
        record.id = QStringLiteral("menu-fixture");
        record.text = QStringLiteral("示例内容");
        record.captured = QDateTime::currentDateTimeUtc();
        clipboard.setRecords({record});
        auto* list = clipboard.findChild<QListView*>();
        QVERIFY(list);
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            launcher.showNear({450, 80});
            clipboard.showNear({80, 80}, false);
            const auto inspect = [&](const QString& name, const auto& open) {
                bool inspected = false;
                bool framed = false;
                bool compact = false;
                QStringList labels;
                QTimer inspectTimer;
                inspectTimer.setSingleShot(true);
                connect(&inspectTimer, &QTimer::timeout, &clipboard, [&] {
                    inspected = true;
                    QPointer<QMenu> menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    if (!menu)
                        return;
                    const bool opaque = QTest::qWaitFor(
                        [&] { return !menu || !menu->isVisible() || menu->windowOpacity() == 1; });
                    if (!menu || !menu->isVisible())
                        return;
                    framed = opaque && (!corners || systemMenuFrame(*menu));
                    compact = true;
                    for (auto* action : menu->actions()) {
                        if (action->isSeparator())
                            continue;
                        labels.append(action->text().section(u'\t', 0, 0));
                        compact &= menu->actionGeometry(action).height() == 30 &&
                                   !action->isIconVisibleInMenu();
                    }
                    menu->setActiveAction(menu->actions().value(0));
                    const auto folder = qEnvironmentVariable("TOOLS_WIDGET_EVIDENCE_DIR");
                    if (!folder.isEmpty()) {
                        QDir().mkpath(folder);
                        renderSurface(*menu).save(
                            QDir(folder).filePath(name + u'-' + mode + QStringLiteral(".png")));
                    }
                    menu->close();
                });
                inspectTimer.start(25);
                open();
                // Clipboard menus now use popup(): wait for the asynchronous
                // inspection instead of relying on a nested exec() event loop.
                QTRY_VERIFY_WITH_TIMEOUT(inspected, 2000);
                inspectTimer.stop();
                QVERIFY2(framed, qPrintable(name));
                QVERIFY2(compact, qPrintable(name));
                QVERIFY(!labels.isEmpty());
                // Let Qt release the popup grab before opening another unrelated menu.
                QTRY_VERIFY_WITH_TIMEOUT(!QApplication::activePopupWidget(), 2000);
                QTest::qWait(400);
                if (name == QStringLiteral("clipboard-context")) {
                    QCOMPARE(labels.size(), 7);
                    QVERIFY(labels.contains(QStringLiteral("回贴")));
                    QVERIFY(labels.contains(QStringLiteral("删除")));
                }
            };
            inspect(QStringLiteral("clipboard-context"), [&] {
                list->customContextMenuRequested(
                    list->visualRect(list->model()->index(0, 0)).center());
            });
            inspect(QStringLiteral("clipboard-toolbar"),
                    [&] { button(clipboard, QStringLiteral("更多操作"))->click(); });
            inspect(QStringLiteral("launcher-toolbar"),
                    [&] { button(launcher, QStringLiteral("启动器菜单"))->click(); });
        }
    }
    void searchTransitionsKeepTheWindowVisibleAndPreserveNavigation() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        panel.setPanelOpacity(95);
        panel.showNear({30, 30}, false);
        auto* textFilter = button(panel, QStringLiteral("文本 / 富文本"));
        QVERIFY(textFilter);
        textFilter->click();
        const auto nativeId = panel.winId();
        const auto geometry = panel.geometry();
        const auto opacity = panel.windowOpacity();
        QSignalSpy visibility(panel.windowHandle(), &QWindow::visibleChanged);
        QSignalSpy dismissed(&panel, &Panel::dismissed);
        auto* animation =
            panel.findChild<QVariantAnimation*>(QStringLiteral("ClipboardSearchAnimation"));
        QVERIFY(animation);
        auto* search = panel.findChild<QLineEdit*>();
        QVERIFY(search);
        panel.showSearch();
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        QCOMPARE(animation->duration(), ClipboardPanel::searchMotionMs);
        // More and Close stay put while searching; only the left controls collapse.
        auto* more = button(panel, QStringLiteral("更多操作"));
        auto* closeButton = button(panel, QStringLiteral("隐藏剪贴板（Esc）"));
        QVERIFY(more && closeButton);
        const auto moreX = more->mapTo(&panel, QPoint()).x();
        const auto closeX = closeButton->mapTo(&panel, QPoint()).x();
        QVERIFY(!panel.windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(
            !(GetWindowLongPtrW(reinterpret_cast<HWND>(nativeId), GWL_EXSTYLE) & WS_EX_NOACTIVATE));
        QTRY_COMPARE_WITH_TIMEOUT(animation->state(), QAbstractAnimation::Stopped, 1000);
        QVERIFY(!textFilter->isVisible());
        QVERIFY(more->isVisible() && closeButton->isVisible());
        QCOMPARE(more->mapTo(&panel, QPoint()).x(), moreX);
        QCOMPARE(closeButton->mapTo(&panel, QPoint()).x(), closeX);
        QVERIFY(search->width() > 200);
        QTest::keyClicks(search, QStringLiteral("clip"));
        QCOMPARE(search->text(), QStringLiteral("clip"));
        animation->setCurrentTime(70);
        panel.closeSearch();
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        QVERIFY(textFilter->isVisible());
        QVERIFY(textFilter->isChecked());
        QVERIFY(search->text().isEmpty());
        QVERIFY(panel.windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(GetWindowLongPtrW(reinterpret_cast<HWND>(nativeId), GWL_EXSTYLE) &
                WS_EX_NOACTIVATE);
        QTRY_COMPARE_WITH_TIMEOUT(animation->state(), QAbstractAnimation::Stopped, 1000);
        QCOMPARE(panel.winId(), nativeId);
        QCOMPARE(panel.geometry(), geometry);
        QCOMPARE(panel.windowOpacity(), opacity);
        QCOMPARE(visibility.size(), 0);
        QCOMPARE(dismissed.size(), 0);
        panel.hide();

        ClipboardPanel reducedMotion(theme, false);
        reducedMotion.showNear({30, 30}, false);
        reducedMotion.showSearch();
        auto* reducedAnimation =
            reducedMotion.findChild<QVariantAnimation*>(QStringLiteral("ClipboardSearchAnimation"));
        QVERIFY(reducedAnimation);
        QCOMPARE(reducedAnimation->state(), QAbstractAnimation::Stopped);
        QVERIFY(reducedMotion.searching());
        reducedMotion.closeSearch();
        QVERIFY(!reducedMotion.searching());
    }
    void clipboardCardsIgnoreLegacySourcesAndSeparateMetadata() {
        Theme theme;
        ClipboardPanel panel(theme);
        auto* list = panel.findChild<QListView*>();
        QVERIFY(list);
        ClipboardRecord record;
        record.id = QStringLiteral("layout-fixture");
        record.text = QStringLiteral("示例内容");
        record.captured = QDateTime(QDate(2025, 10, 22), QTime(12, 0), QTimeZone::UTC);
        auto render = [&] {
            QImage image(360, 86, QImage::Format_ARGB32_Premultiplied);
            image.fill(theme.color(QStringLiteral("surface")));
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = image.rect();
            option.font = qApp->font();
            option.state = QStyle::State_Enabled;
            list->itemDelegate()->paint(&painter, option, list->model()->index(0, 0));
            painter.end();
            return image;
        };
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            record.source = QStringLiteral("不应显示的来源 A");
            panel.setRecords({record});
            const auto first = render();
            record.source = QStringLiteral("不应显示的来源 B");
            panel.setRecords({record});
            QCOMPARE(render(), first);
            bool coloredIndex = false;
            bool neutralTime = false;
            const auto cardColor = theme.color(QStringLiteral("surface-subtle"));
            for (int y = 58; y < 76; ++y) {
                for (int x = 18; x < 48; ++x) {
                    const auto c = first.pixelColor(x, y);
                    coloredIndex |= c.blue() > c.red() + 25;
                }
                for (int x = 245; x < 315; ++x) {
                    const auto c = first.pixelColor(x, y);
                    neutralTime |= std::abs(c.red() - c.blue()) < 10 &&
                                   std::abs(c.lightness() - cardColor.lightness()) > 40;
                }
            }
            QVERIFY(coloredIndex);
            QVERIFY(neutralTime);
        }
    }
    void clipboardCardActionsHaveStableHitsAndNeverPaste() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        ClipboardRecord record;
        record.id = QStringLiteral("action-fixture");
        record.text = QStringLiteral("示例内容");
        panel.setRecords({record});
        panel.showNear({30, 30}, false);
        auto* list = panel.findChild<QListView*>();
        QVERIFY(list);
        QApplication::processEvents();
        const auto row = list->visualRect(list->model()->index(0, 0));
        QCOMPARE(row.height(), 86);
        QSignalSpy flags(&panel, &ClipboardPanel::toggleFlagRequested);
        QSignalSpy paste(&panel, &ClipboardPanel::pasteRequested);
        const QPoint star(row.right() - 73, row.top() + 22);
        const QPoint pin = star + QPoint(22, 0);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, star);
        QCOMPARE(flags.size(), 1);
        QCOMPARE(flags.last().at(0).toString(), record.id);
        QCOMPARE(flags.last().at(1).value<ClipFlag>(), ClipFlag::Favorite);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pin);
        QCOMPARE(flags.size(), 2);
        QCOMPARE(flags.last().at(1).value<ClipFlag>(), ClipFlag::Pinned);
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, star);
        QCOMPARE(paste.size(), 0);
        QTest::mousePress(list->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
        QTest::mouseRelease(list->viewport(), Qt::LeftButton, Qt::NoModifier, star);
        QCOMPARE(flags.size(), 2);
        panel.hide();
    }
    void opacityPreviewCancelAndSave() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        const Settings settings = Settings::defaults();
        Panel launcher(theme, QStringLiteral("fixture launcher"));
        ClipboardPanel clipboard(theme);
        launcher.setPanelOpacity(settings.launcherOpacity);
        clipboard.setPanelOpacity(settings.clipboardOpacity);
        auto closeEnough = [](qreal a, qreal b) { return std::abs(a - b) <= 1.0 / 255; };
        bool saved = false;
        SettingsDialog cancelled(theme, settings, QStringLiteral("fixture"),
                                 [&](const Settings&, SettingsDialog::Completion done) {
                                     saved = true;
                                     done({});
                                 });
        connect(&cancelled, &SettingsDialog::panelOpacityPreview, this, [&](int a, int b) {
            launcher.setPanelOpacity(a);
            clipboard.setPanelOpacity(b);
        });
        cancelled.findChild<QSpinBox*>(QStringLiteral("launcherOpacity"))->setValue(80);
        cancelled.findChild<QSpinBox*>(QStringLiteral("clipboardOpacity"))->setValue(70);
        QVERIFY(closeEnough(launcher.windowOpacity(), 0.8));
        QVERIFY(closeEnough(clipboard.windowOpacity(), 0.7));
        launcher.showNear({440, 30});
        clipboard.showNear({30, 30}, false);
        auto nativeAlpha = [](QWidget& widget, int expected) {
            // Per-pixel layered windows use UpdateLayeredWindow, not the LWA_ALPHA API.
            const bool layered =
                GetWindowLongPtrW(reinterpret_cast<HWND>(widget.winId()), GWL_EXSTYLE) &
                WS_EX_LAYERED;
            return layered && widget.windowHandle() &&
                   std::abs(qRound(widget.windowHandle()->opacity() * 255) - expected) <= 1;
        };
        QVERIFY(nativeAlpha(launcher, 204));
        QVERIFY(nativeAlpha(clipboard, 178));
        clipboard.showSearch();
        clipboard.closeSearch();
        QVERIFY(closeEnough(clipboard.windowOpacity(), 0.7));
        cancelled.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!saved);
        QVERIFY(closeEnough(launcher.windowOpacity(), 0.95));
        QVERIFY(closeEnough(clipboard.windowOpacity(), 0.95));
        QVERIFY(nativeAlpha(clipboard, 242));
        launcher.hide();
        clipboard.hide();
        Settings persisted;
        SettingsDialog accepted(theme, settings, QStringLiteral("fixture"),
                                [&](const Settings& next, SettingsDialog::Completion done) {
                                    persisted = next;
                                    saved = true;
                                    done({});
                                });
        accepted.findChild<QSpinBox*>(QStringLiteral("launcherOpacity"))->setValue(88);
        accepted.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(saved);
        QCOMPARE(persisted.launcherOpacity, 88);
        QCOMPARE(persisted.clipboardOpacity, 95);
    }
    void lucideAssetsBackEverySymbolAtMultipleScales() {
        Theme theme;
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            for (int raw = 0; raw <= static_cast<int>(Symbol::ChevronRight); ++raw) {
                const auto symbol = static_cast<Symbol>(raw);
                const auto asset = Theme::iconAsset(symbol);
                QVERIFY2(!asset.isEmpty(), qPrintable(QString::number(raw)));
                QVERIFY2(QFile::exists(QStringLiteral(":/mt/icons/lucide/%1.svg").arg(asset)),
                         qPrintable(asset));
                const auto icon = theme.icon(symbol);
                QVERIFY(!icon.isNull());
                for (qreal scale : {1.0, 1.5, 2.0}) {
                    const auto image = icon.pixmap(QSize(18, 18), scale).toImage();
                    bool painted = false;
                    for (int y = 0; y < image.height() && !painted; ++y)
                        for (int x = 0; x < image.width(); ++x)
                            if (image.pixelColor(x, y).alpha() > 0) {
                                painted = true;
                                break;
                            }
                    QVERIFY(painted);
                }
            }
        }
    }
    void themePreservesSystemFontAndKeepsLayerContrast() {
        const auto original = qApp->font();
        Theme theme;
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            QCOMPARE(qApp->font(), original);
            QVERIFY(theme.color(QStringLiteral("surface-subtle")) !=
                    theme.color(QStringLiteral("surface")));
        }
        theme.apply(QStringLiteral("dark"));
        QVERIFY(theme.color(QStringLiteral("surface")).lightness() >= 35);
        QVERIFY(theme.color(QStringLiteral("text")).lightness() >= 220);
    }
    void closeDoesNotVetoApplicationQuit() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        Panel panel(theme, QStringLiteral("fixture"));
        panel.showNear(QPoint(30, 30));
        QCloseEvent close;
        QApplication::sendEvent(&panel, &close);
        QVERIFY(close.isAccepted());
        QVERIFY(!panel.isVisible());
    }
    void filterIsToggleAndSearchPreservesIt() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        panel.showNear({30, 30}, false);
        QSignalSpy changes(&panel, &ClipboardPanel::queryChanged);
        auto* text = button(panel, QStringLiteral("文本 / 富文本"));
        QVERIFY(text);
        text->click();
        QVERIFY(text->isChecked());
        QCOMPARE(changes.last().at(1).toInt(), 0);
        panel.showSearch();
        QVERIFY(panel.searching());
        QTRY_VERIFY_WITH_TIMEOUT(!text->isVisible(), 1000);
        auto* search = panel.findChild<QLineEdit*>();
        QVERIFY(search);
        search->setText(QStringLiteral("合成搜索"));
        panel.closeSearch();
        QVERIFY(!panel.searching());
        QVERIFY(text->isVisible());
        QVERIFY(text->isChecked());
        QVERIFY(search->text().isEmpty());
        QCOMPARE(changes.last().at(1).toInt(), 0);
        text->click();
        QVERIFY(!text->isChecked());
        QCOMPARE(changes.last().at(1).toInt(), -1);
        panel.hide();
    }
    void clipboardHasBottomInsetAndNoVisibleTitle() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        panel.showNear({30, 30}, false);
        QApplication::sendPostedEvents();
        const auto* list = panel.findChild<QListView*>();
        QVERIFY(list);
        QCOMPARE(panel.size(), QSize(380, 540));
        const auto* toolbar = panel.findChild<QWidget*>(QStringLiteral("Toolbar"));
        QVERIFY(toolbar);
        QVERIFY(list->mapTo(&panel, QPoint(0, 0)).y() - toolbar->geometry().bottom() - 1 >= 8);
        QVERIFY(panel.height() - (list->mapTo(&panel, QPoint(0, list->height())).y()) >= 8);
        QVERIFY(!button(panel, QStringLiteral("全部")));
        ClipboardRecords records;
        for (int i = 0; i < 15; ++i) {
            ClipboardRecord record;
            record.id = QString::number(i);
            record.text = QStringLiteral("滚动条布局验证");
            records.append(record);
        }
        panel.setRecords(records);
        QApplication::processEvents();
        auto* scroll = list->verticalScrollBar();
        QVERIFY(scroll->isVisible());
        QCOMPARE(scroll->width(), 8);
        const auto image = scroll->grab().toImage();
        int maximumPaintedWidth = 0;
        for (int y = 0; y < image.height(); ++y) {
            int painted = 0;
            const auto background = image.pixelColor(0, y);
            for (int x = 0; x < image.width(); ++x)
                painted += image.pixelColor(x, y) != background;
            maximumPaintedWidth = qMax(maximumPaintedWidth, painted);
        }
        QVERIFY(maximumPaintedWidth > 0);
        QVERIFY(maximumPaintedWidth <= qCeil(4 * image.devicePixelRatio()));
    }
    void launcherGeometryAndBlankSlots() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        LauncherPanel panel(theme, service);
        panel.keepOpen = true;
        panel.showNear({30, 30});
        panel.refresh();
        QApplication::sendPostedEvents();
        int count = 0;
        QToolButton* first = nullptr;
        for (auto* tile : panel.findChildren<QToolButton*>())
            if (tile->property("slot").isValid()) {
                ++count;
                QCOMPARE(tile->size(), QSize(72, 62));
                if (tile->property("slot").toInt() == 0)
                    first = tile;
            }
        QCOMPARE(count, 28);
        QCOMPARE(panel.size(), QSize(318, 540));
        QVERIFY(first);
        QCOMPARE(first->mapTo(&panel, QPoint(0, 0)), QPoint(9, Theme::headerHeight + 8));
        // Tabs align with the grid and stay clear of the rounded corners, so the
        // selected underline of an edge tab never bends.
        auto* tabs = panel.findChild<QTabBar*>();
        QVERIFY(tabs);
        const QRect tabArea(tabs->mapTo(&panel, QPoint(0, 0)), tabs->size());
        QCOMPARE(tabArea.left(), 9);
        QCOMPARE(tabArea.right(), panel.width() - 10);
        QCOMPARE(tabArea.bottom(), panel.height() - 1);
        QVERIFY(first->text().isEmpty());
        // The grid size is a setting; the window and the tiles follow it.
        QVERIFY(service.setColumns(6));
        QTRY_VERIFY(!service.busy());
        panel.setGrid(service.catalog().columns, 3);
        QApplication::sendPostedEvents();
        count = 0;
        QToolButton* last = nullptr;
        for (auto* tile : panel.findChildren<QToolButton*>())
            if (tile->property("slot").isValid()) {
                ++count;
                if (tile->property("slot").toInt() == 17)
                    last = tile;
            }
        QCOMPARE(count, 18);
        QCOMPARE(panel.size(), QSize(18 + 6 * 72 + 5 * 4, Theme::headerHeight + 16 + 3 * 62 + 2 * 4 + 30));
        QVERIFY(last);
        QCOMPARE(last->mapTo(&panel, QPoint(0, 0)),
                 QPoint(9 + 5 * 76, Theme::headerHeight + 8 + 2 * 66));
        QCOMPARE(panel.gridColumns(), 6);
        QCOMPARE(panel.gridRows(), 3);
    }
    void overflowingCategoriesScrollWithArrowsAtBothEnds() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        LauncherPanel panel(theme, service);
        panel.keepOpen = true;
        panel.showNear({30, 30});
        QApplication::sendPostedEvents();
        QToolButton* left = nullptr;
        QToolButton* right = nullptr;
        const auto find = [&] {
            for (auto* b : panel.findChildren<QToolButton*>(QStringLiteral("TabScroll")))
                (b->accessibleName() == QStringLiteral("向左滚动分类") ? left : right) = b;
        };
        find();
        QVERIFY(left && right);
        QVERIFY(left->isHidden() && right->isHidden()); // four categories fit
        for (int i = 0; i < 6; ++i)
            QVERIFY(service.addCategory(QStringLiteral("阿道夫阿道夫%1").arg(i)));
        QTRY_VERIFY(!service.busy());
        QApplication::sendPostedEvents();
        QApplication::processEvents();
        QVERIFY(left->isVisible() && right->isVisible());
        auto* tabs = panel.findChild<QTabBar*>();
        QVERIFY(tabs);
        const auto at = [&](QWidget* w) { return QRect(w->mapTo(&panel, QPoint()), w->size()); };
        // One arrow at each end, the tabs between them, no gap at the right.
        QCOMPARE(at(left).left(), 9);
        QCOMPARE(at(right).right(), panel.width() - 10);
        QCOMPARE(at(tabs).left(), at(left).right() + 1);
        QCOMPARE(at(tabs).right(), at(right).left() - 1);
        // The newest category is selected and scrolled into view.
        const auto current = tabs->tabRect(tabs->currentIndex());
        QVERIFY2(current.right() <= tabs->width() && current.left() >= 0,
                 qPrintable(QStringLiteral("%1-%2 of %3").arg(current.left()).arg(current.right()).arg(tabs->width())));
        QVERIFY(left->isEnabled());
        QVERIFY(!right->isEnabled());
        // Scrolling left moves the tabs right until the first one is reachable.
        const int before = tabs->tabRect(0).left();
        left->click();
        QApplication::processEvents();
        QVERIFY(tabs->tabRect(0).left() > before);
        for (int i = 0; i < 20 && left->isEnabled(); ++i) {
            left->click();
            QApplication::processEvents();
        }
        QCOMPARE(tabs->tabRect(0).left(), 0);
        QVERIFY(!left->isEnabled());
        QVERIFY(right->isEnabled());
        // Back to four categories: the arrows go away.
        const auto ids = service.catalog().categories;
        for (int i = 4; i < ids.size(); ++i)
            QVERIFY(service.removeCategory(ids[i].id));
        QTRY_VERIFY(!service.busy());
        QApplication::sendPostedEvents();
        QApplication::processEvents();
        QVERIFY(left->isHidden() && right->isHidden());
    }
    void settingsSaveKeepsWindowAndCancelUsesLatestBaseline() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        SettingsDialog::Completion finish;
        Settings persisted;
        SettingsDialog dialog(theme, Settings::defaults(), QStringLiteral("fixture"),
            [&](const Settings& next, SettingsDialog::Completion done) { persisted = next; finish = std::move(done); });
        dialog.show();
        auto* value = dialog.findChild<QSpinBox*>(QStringLiteral("launcherOpacity"));
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        value->setValue(88);
        buttons->button(QDialogButtonBox::Save)->click();
        QVERIFY(finish && !dialog.isEnabled());
        dialog.reject();
        QVERIFY(dialog.isVisible());
        finish({});
        QCOMPARE(persisted.launcherOpacity, 88);
        QVERIFY(dialog.isVisible() && dialog.isEnabled());
        QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
        QSignalSpy opacity(&dialog, &SettingsDialog::panelOpacityPreview);
        value->setValue(71);
        buttons->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(opacity.last().at(0).toInt(), 88);
        QVERIFY(!dialog.isVisible());
    }
    void settingsFailurePreservesDraftAndCloseCanBeCancelled() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        SettingsDialog dialog(theme, Settings::defaults(), QStringLiteral("fixture"),
            [](const Settings&, SettingsDialog::Completion done) { done(std::unexpected(QStringLiteral("模拟磁盘写入失败"))); });
        dialog.show();
        auto* value = dialog.findChild<QSpinBox*>(QStringLiteral("launcherOpacity")); value->setValue(80);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(value->value(), 80);
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("SettingsError"))->text().contains(QStringLiteral("写入失败")));
        QVERIFY(dialog.isVisible());
        dialog.close();
        QPointer<QMessageBox> confirmation = dialog.findChild<QMessageBox*>(QStringLiteral("unsavedSettingsConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        confirmation->defaultButton()->click();
        QTRY_VERIFY(confirmation.isNull());
        QVERIFY(dialog.isVisible()); QCOMPARE(value->value(), 80);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    }
    void settingsPagesFitAndRenderBothThemes() {
        qApp->setFont(DesktopServices::systemUiFont());
        Theme theme;
        SettingsDialog dialog(theme, Settings::defaults(), QStringLiteral("fixture/settings-preview"),
            [](const Settings&, SettingsDialog::Completion done) { done({}); });
        auto* pages = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
        QCOMPARE(pages->count(), 7);
        QVERIFY(!dialog.findChild<QWidget*>(QStringLiteral("language")));
        dialog.show();
        const auto folder = qEnvironmentVariable("MT_SETTINGS_RENDER_DIR");
        if (!folder.isEmpty()) QVERIFY(QDir().mkpath(folder));
        for (const auto& mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
            for (auto* choice : dialog.findChildren<QPushButton*>(QStringLiteral("ThemeChoice")))
                if (choice->property("themeValue").toString() == mode) choice->click();
            dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            for (const auto size : {QSize(800, 600), QSize(1000, 760)}) {
                dialog.resize(size);
                for (int page = 0; page < pages->count(); ++page) {
                    for (auto* nav : dialog.findChildren<QPushButton*>(QStringLiteral("SettingsNav")))
                        if (nav->property("pageIndex").toInt() == page) nav->click();
                    QApplication::processEvents();
                    auto* tabs = pages->currentWidget()->findChild<QTabWidget*>();
                    const int count = tabs ? tabs->count() : 1;
                    for (int tab = 0; tab < count; ++tab) {
                        if (tabs) tabs->setCurrentIndex(tab);
                        QApplication::processEvents();
                        for (auto* scroll : pages->currentWidget()->findChildren<QScrollArea*>())
                            if (scroll->isVisible()) QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                        if (!folder.isEmpty() && size.width() == 1000)
                            QVERIFY(renderSurface(dialog).save(QDir(folder).filePath(
                                QStringLiteral("settings-%1-%2-%3.png").arg(mode).arg(page).arg(tab))));
                    }
                }
            }
        }
    }
    void themeCancelRestoresOriginal() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        Settings settings = Settings::defaults();
        bool saved = false;
        SettingsDialog dialog(theme, settings, QStringLiteral("fixture"),
                              [&](const Settings&, SettingsDialog::Completion done) {
                                  saved = true;
                                  done({});
                              });
        QPushButton* light = nullptr;
        for (auto* button : dialog.findChildren<QPushButton*>(QStringLiteral("ThemeChoice")))
            if (button->property("themeValue").toString() == QStringLiteral("light")) light = button;
        QVERIFY(light);
        light->click();
        QCOMPARE(theme.name(), QStringLiteral("light"));
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(theme.name(), QStringLiteral("dark"));
        QVERIFY(!saved);
    }
    void lockSettingsSaveProtectedPasswordAndExplainModes() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        Settings saved;
        bool applied = false;
        SettingsDialog dialog(theme, Settings::defaults(), QStringLiteral("fixture"),
                              [&](const Settings& next, SettingsDialog::Completion done) {
                                  saved = next;
                                  applied = true;
                                  done(next.validate());
                              });
        auto* enabled = dialog.findChild<QCheckBox*>(QStringLiteral("lockEnabled"));
        auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("requirePassword"));
        auto* password = dialog.findChild<QLineEdit*>(QStringLiteral("lockPassword"));
        auto* note = dialog.findChild<QLabel*>(QStringLiteral("lockModeNote"));
        QVERIFY(enabled && enabled->isEnabled() && mode && password && note);
        QVERIFY(note->text().contains(QStringLiteral("简单模式")));
        enabled->setChecked(true);
        mode->setChecked(true);
        QVERIFY(password->isEnabled());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(note->text().contains(QStringLiteral("不会解锁")));
        const auto renderFolder = qEnvironmentVariable("MT_LOCK_RENDER_DIR");
        if (!renderFolder.isEmpty()) {
            QVERIFY(QDir().mkpath(renderFolder));
            dialog.findChild<QStackedWidget*>(QStringLiteral("settingsPages"))->setCurrentIndex(5);
            dialog.show();
            for (const auto& modeName : {QStringLiteral("dark"), QStringLiteral("light")}) {
                theme.apply(modeName);
                QTest::qWait(50);
                QVERIFY(renderSurface(dialog).save(QDir(renderFolder).filePath(
                    QStringLiteral("settings-lock-") + modeName + QStringLiteral(".png"))));
            }
            dialog.hide();
        }
        password->setText(QStringLiteral("Ab123"));
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(applied && saved.requirePassword && saved.lockEnabled);
        QVERIFY(!saved.encode().contains("Ab123"));
        const auto decoded = revealLockPassword(saved.protectedPassword);
        QVERIFY(decoded);
        QCOMPARE(*decoded, QStringLiteral("Ab123"));
        bool keptPassword = false;
        SettingsDialog again(theme, saved, QStringLiteral("fixture"),
                             [&](const Settings& next, SettingsDialog::Completion done) {
                                 keptPassword = next.protectedPassword == saved.protectedPassword;
                                 done({});
                             });
        QVERIFY(again.findChild<QLineEdit*>(QStringLiteral("lockPassword"))->text().isEmpty());
        again.findChild<QCheckBox*>(QStringLiteral("lockPaused"))->setChecked(true);
        again.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(keptPassword);
        QVERIFY(!revealLockPassword(QStringLiteral("invalid")));
        QVERIFY(!revealLockPassword(QStringLiteral("dpapi:broken")));
    }
    void caretPlacementStaysOnTheCaretScreen() {
        // DISPLAY2 of the reported setup: 125 % screen whose logical area is
        // 1916,1080 1536x864, directly below a 100 % screen.
        const QRect area(1916, 1080, 1536, 864);
        const QSize panel(380, 540);
        // Room below: just under the caret line.
        QCOMPARE(placeBesideCaret(QRect(2000, 1100, 2, 20), panel, area), QPoint(2000, 1124));
        // No room below but room above: flipped above the line.
        QCOMPARE(placeBesideCaret(QRect(2000, 1800, 2, 20), panel, area), QPoint(2000, 1800 - 4 - 540));
        // Room on neither side (the reported case): beside the caret on the same
        // screen, never moved onto the screen above and never over the line.
        const QRect line(2502, 1414, 2, 23);
        const auto middle = placeBesideCaret(line, panel, area);
        QVERIFY(area.contains(QRect(middle, panel)));
        QCOMPARE(middle.x(), line.right() + 1 + 4);
        QVERIFY(!QRect(middle, panel).intersects(line));
        // Near the right edge: right-aligned with the caret, still on screen.
        const auto right = placeBesideCaret(QRect(3440, 1100, 2, 20), panel, area);
        QCOMPARE(right.x(), 3441 + 1 - panel.width());
        QVERIFY(area.contains(QRect(right, panel)));
    }
    void reopenedPanelStartsAtNewestRecord() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme, false);
        const auto record = [](const char* id) {
            ClipboardRecord r;
            r.id = QString::fromLatin1(id);
            r.text = r.id;
            return r;
        };
        panel.setRecords({record("b"), record("a")});
        panel.handleKey(Qt::Key_Down);
        auto* list = panel.findChild<QListView*>();
        QCOMPARE(list->currentIndex().row(), 1);
        // A refresh while open keeps the place...
        panel.setRecords({record("c"), record("b"), record("a")});
        QCOMPARE(list->currentIndex().row(), 2);
        // ...but opening again starts at the newest record.
        panel.selectNewestOnNextRecords();
        panel.setRecords({record("d"), record("c"), record("b"), record("a")});
        QCOMPARE(list->currentIndex().row(), 0);
    }
    void colorValuesGetASwatch() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme, false);
        ClipboardRecord color;
        color.id = QStringLiteral("color");
        color.text = QStringLiteral("#E5534B");
        panel.setRecords({color});
        panel.showNear({30, 30}, false);
        auto* list = panel.findChild<QListView*>();
        const auto row = list->visualRect(list->model()->index(0, 0));
        const auto image = list->viewport()->grab().toImage();
        const auto dpr = image.devicePixelRatio();
        if (const auto dir = qEnvironmentVariable("TOOLS_WIDGET_EVIDENCE_DIR"); !dir.isEmpty())
            image.save(QDir(dir).filePath(QStringLiteral("clipboard-color-swatch.png")));
        // Card inset 6/3, content inset 12/10; the 14px chip sits at the line start.
        const QPoint chip(row.left() + 6 + 12 + 7, row.top() + 3 + 10 + 9);
        QCOMPARE(image.pixelColor(qRound(chip.x() * dpr), qRound(chip.y() * dpr)).rgb(),
                 QColor(0xE5, 0x53, 0x4B).rgb());
    }
    void clipboardBehaviorSettingsDrivePanel() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme, false);
        ClipboardRecord link;
        link.id = QStringLiteral("link");
        link.text = QStringLiteral("https://example.com");
        ClipboardRecord body;
        body.id = QStringLiteral("body");
        QStringList lines;
        for (int i = 0; i < 200; ++i)
            lines << QStringLiteral("line %1").arg(i);
        body.text = lines.join(u'\n');
        panel.setRecords({link, body});
        panel.showNear(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(20, 60), false);
        QSignalSpy previews(&panel, &ClipboardPanel::previewRequested);
        QSignalSpy opens(&panel, &ClipboardPanel::openRequested);
        auto* viewport = panel.findChild<QListView*>()->viewport();
        const auto firstRow = panel.findChild<QListView*>()->visualRect(
            panel.findChild<QListView*>()->model()->index(0, 0)).center();

        ClipboardBehavior off;
        off.hoverPreview = false;
        off.spacePreview = false;
        off.openKey = 0;
        off.openButton = Qt::NoButton;
        panel.setBehavior(off);
        QTest::mouseMove(viewport, firstRow);
        QTest::qWait(300);
        panel.handleKey(Qt::Key_Space);
        panel.handleKey(Qt::Key_Space, false, false, false);
        panel.handleKey(Qt::Key_O);
        QTest::mouseClick(viewport, Qt::MiddleButton, {}, firstRow);
        QCOMPARE(previews.size(), 0);
        QCOMPARE(opens.size(), 0);

        ClipboardBehavior on;
        on.openKey = Qt::Key_K;
        on.openButton = Qt::MiddleButton;
        on.previewLeft = false;
        panel.setBehavior(on);
        panel.handleKey(Qt::Key_O);
        QCOMPARE(opens.size(), 0);
        panel.handleKey(Qt::Key_K);
        QCOMPARE(opens.size(), 1);
        QTest::mouseClick(viewport, Qt::MiddleButton, {}, firstRow);
        QCOMPARE(opens.size(), 2);
        QCOMPARE(opens.last().at(0).toString(), QStringLiteral("link"));

        // The preview opens on the preferred right side and side buttons scroll it.
        panel.handleKey(Qt::Key_Down);
        panel.handleKey(Qt::Key_Space);
        QCOMPARE(previews.size(), 1);
        panel.displayPreview(body, previews.last().at(1).toULongLong());
        auto* preview = panel.findChild<Panel*>();
        QVERIFY(preview && preview->isVisible());
        QVERIFY(preview->x() >= panel.geometry().right());
        auto* text = preview->findChild<QPlainTextEdit*>();
        QVERIFY(text);
        QCOMPARE(text->verticalScrollBar()->value(), 0);
        for (int i = 0; i < 3; ++i)
            panel.sideScroll(1);
        QTRY_VERIFY_WITH_TIMEOUT(text->verticalScrollBar()->value() > 0, 1000);
        panel.handleKey(Qt::Key_Space, false, false, false);
        QVERIFY(!preview->isVisible());
    }
    void releasedPreviewRejectsLatePayload() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        ClipboardRecord record;
        record.id = QStringLiteral("fixture");
        record.text = QStringLiteral("fixture");
        panel.setRecords({record});
        panel.showNear({30, 30}, false);
        QSignalSpy requests(&panel, &ClipboardPanel::previewRequested);
        panel.handleKey(Qt::Key_Space);
        QCOMPARE(requests.size(), 1);
        const auto token = requests.front().at(1).toULongLong();
        panel.handleKey(Qt::Key_Space, false, false, false);
        panel.displayPreview(record, token);
        auto* preview = panel.findChild<Panel*>();
        QVERIFY(preview);
        QVERIFY(!preview->isVisible());
    }
    void previewDocksBesidePanelWithoutFocusAndPanelsShareHeader() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme);
        ClipboardRecord record;
        record.id = QStringLiteral("fixture");
        record.text = QStringLiteral("第一行\n第二行 https://example.com");
        panel.setRecords({record});
        panel.showNear({520, 40}, false);
        QSignalSpy requests(&panel, &ClipboardPanel::previewRequested);
        panel.handleKey(Qt::Key_Space);
        QCOMPARE(requests.size(), 1);
        panel.displayPreview(record, requests.front().at(1).toULongLong());
        auto* preview = panel.findChild<Panel*>();
        QVERIFY(preview && preview->isVisible());
        QVERIFY(preview->windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(!preview->geometry().intersects(panel.geometry()));
        QVERIFY(preview->height() <= panel.height());
        QCOMPARE(preview->y(), panel.y());
        // Holding Space: Windows auto-repeat sends release/press pairs to Qt and
        // repeated presses through the hook; neither may close or re-request.
        for (int i = 0; i < 5; ++i) {
            QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier,
                                    QStringLiteral(" "), true);
            QKeyEvent repeatPress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                                  QStringLiteral(" "), true);
            QApplication::sendEvent(&panel, &repeatRelease);
            QApplication::sendEvent(&panel, &repeatPress);
            panel.handleKey(Qt::Key_Space);
        }
        QVERIFY(preview->isVisible());
        QCOMPARE(requests.size(), 1);
        panel.handleKey(Qt::Key_Space, false, false, false);
        QVERIFY(!preview->isVisible());

        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        LauncherPanel launcher(theme, service);
        const auto headerOf = [](QWidget& w) {
            for (auto* child : w.findChildren<QWidget*>(QStringLiteral("Toolbar")))
                if (child->parentWidget() == &w)
                    return child->height();
            return -1;
        };
        QCOMPARE(headerOf(launcher), Theme::headerHeight);
        QCOMPARE(headerOf(panel), Theme::headerHeight);
        QCOMPARE(launcher.height(), panel.height());
    }
    void bothThemesSharePaletteRoles() {
        Theme theme;
        for (const auto& name : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(name);
            QCOMPARE(qApp->palette().color(QPalette::Window),
                     theme.color(QStringLiteral("surface")));
            QCOMPARE(qApp->palette().color(QPalette::Text), theme.color(QStringLiteral("text")));
            QVERIFY(theme.color(QStringLiteral("surface")).isValid());
        }
    }
};
QTEST_MAIN(WidgetTests)
#include "WidgetTests.moc"
