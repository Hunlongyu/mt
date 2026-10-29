#include "infrastructure/i18n/I18n.h"
#include "infrastructure/storage/HLaunchImport.h"
#include "modules/launcher/LauncherPanel.h"
#include "platform/windows/ShellJobs.h"
#include "ui/components/Theme.h"
#include "ui/shell/SettingsDialog.h"
#include <QApplication>
#include <QBuffer>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtPlugin>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <cstdio>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
using namespace mt;
namespace {
LaunchItem item(const QString& name) {
    LaunchItem i;
    i.id = LauncherCatalog::newId();
    i.name = name;
    i.target = name + QStringLiteral(".exe");
    return i;
}
QByteArray bytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
bool write(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
QToolButton* tile(LauncherPanel& panel, int slot) {
    for (auto* button : panel.findChildren<QToolButton*>())
        if (button->property("slot").isValid() && button->property("slot").toInt() == slot)
            return button;
    return nullptr;
}
void key(QWidget* target, int code, Qt::KeyboardModifiers modifiers = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(target, &event);
}
QStringList tabIds(QTabBar* tabs) {
    QStringList ids;
    for (int i = 0; i < tabs->count(); ++i)
        ids.append(tabs->tabData(i).toString());
    return ids;
}
QStringList categoryIds(const LauncherCatalog& catalog) {
    QStringList ids;
    for (const auto& category : catalog.categories)
        ids.append(category.id);
    return ids;
}
void dragTab(QTabBar* tabs, QPoint position) {
    QMouseEvent move(QEvent::MouseMove, position, tabs->mapToGlobal(position), Qt::NoButton,
                     Qt::LeftButton, {});
    QApplication::sendEvent(tabs, &move);
}
class StyleChanges final : public QObject {
  public:
    int count = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::StyleChange)
            ++count;
        return false;
    }
};
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
// Deterministic Qt focus transitions for this unit test, independent of other
// applications competing for the Windows foreground during the test run.
void activateForTest(QWidget* widget) {
    QApplication::setActiveWindow(widget);
}
QT_WARNING_POP
// The test executable plays the helper so ShellJobs exercises real process/pipe
// isolation without launching Explorer, applications, or accessing real user data.
int helper() {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 1;
    const auto request = QJsonDocument::fromJson(input.readAll()).object();
    QJsonObject response{{QStringLiteral("ok"), true}};
    if (request.value(QStringLiteral("operation")).toString() == QStringLiteral("icon")) {
        QImage image(request.value(QStringLiteral("source")).toString());
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        const bool ok = !image.isNull() && image.save(&buffer, "PNG");
        response[QStringLiteral("ok")] = ok;
        response[QStringLiteral("png")] = QString::fromLatin1(png.toBase64());
    } else if (request.value(QStringLiteral("operation")).toString() == QStringLiteral("reveal")) {
        QThread::msleep(250);
        response[QStringLiteral("ok")] =
            request.value(QStringLiteral("target")).toString() == QStringLiteral("slow");
        response[QStringLiteral("error")] = QStringLiteral("fixture reveal failure");
    }
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly))
        return 1;
    const auto payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    return output.write(payload) == payload.size() && output.flush() ? 0 : 1;
}
} // namespace
class LauncherRegressionTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void init() {
    }
    void cleanup() {
    }
    void initialFocusRoutesGridNavigation() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        activateForTest(&panel);
        QTRY_COMPARE(QApplication::focusWidget(), tile(panel, 0));
        key(QApplication::focusWidget(), Qt::Key_Right);
        QCOMPARE(QApplication::focusWidget(), tile(panel, 1));
        key(QApplication::focusWidget(), Qt::Key_Down);
        QCOMPARE(QApplication::focusWidget(), tile(panel, 5));
        key(QApplication::focusWidget(), Qt::Key_PageDown);
        QVERIFY(panel.windowTitle().contains(tx(u"MT · 启动面板 · 第 %1 页").arg(2)));
        key(QApplication::focusWidget(), Qt::Key_PageUp);
        auto* tabs = panel.findChild<QTabBar*>();
        const int initial = tabs->currentIndex();
        key(QApplication::focusWidget(), Qt::Key_Tab);
        QCOMPARE(tabs->currentIndex(), (initial + 1) % tabs->count());
        key(QApplication::focusWidget(), Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(tabs->currentIndex(), initial);
        QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
        QVERIFY(QApplication::focusWidget()->property("slot").isValid());
        key(QApplication::focusWidget(), Qt::Key_Right);
        const auto selected = QApplication::focusWidget();
        panel.hide();
        panel.showNear({20, 20});
        activateForTest(&panel);
        QTRY_COMPARE(QApplication::focusWidget(), selected);
    }
    void categoryDragSurvivesSaveBeforeRelease_data() {
        QTest::addColumn<bool>("current");
        QTest::newRow("current category") << true;
        QTest::newRow("different category") << false;
    }
    void categoryDragSurvivesSaveBeforeRelease() {
        QFETCH(bool, current);
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        const auto entry = item(QStringLiteral("stat"));
        QVERIFY(catalog.put(2, 0, entry));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        QTest::qWait(30);
        auto* tabs = panel.findChild<QTabBar*>();
        const auto original = tabIds(tabs);
        const int from = current ? 2 : 0;
        const int destination = current ? 0 : 2;
        const auto dragged = original[from];
        tabs->setTabToolTip(from, QStringLiteral("drag marker"));
        QSignalSpy moved(tabs, &QTabBar::tabMoved);
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(from).center());
        const auto release = qScopeGuard([&] {
            QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(destination).center());
        });
        dragTab(tabs, tabs->tabRect(1).center());
        QVERIFY(!moved.isEmpty());
        service.recordLaunch(entry.id);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(tabs->tabData(1).toString(), dragged);
        QCOMPARE(tabs->tabToolTip(1), QStringLiteral("drag marker"));
        QCOMPARE(categoryIds(service.catalog()), original); // order saves only after release
        dragTab(tabs, tabs->tabRect(destination).center());
        auto expected = original;
        expected.move(from, destination);
        QCOMPARE(tabIds(tabs), expected);
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(destination).center());
        QTRY_COMPARE(categoryIds(service.catalog()), expected);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(tabIds(tabs), expected);
        QCOMPARE(service.catalog().items[entry.id].launchCount, 1);
    }
    void overlappingCategoryDragsRecoverFromFailedSave() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        std::atomic<bool> failNext{true};
        LauncherService service(path, nullptr, [&](const auto& catalog, const auto& destination) {
            if (failNext.exchange(false)) {
                QThread::msleep(100);
                return LauncherService::Result(std::unexpected(QStringLiteral("fixture failure")));
            }
            return catalog.save(destination);
        });
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        QTest::qWait(30);
        auto* tabs = panel.findChild<QTabBar*>();
        const auto original = tabIds(tabs);
        QSignalSpy failures(&service, &LauncherService::failed);
        const auto release = qScopeGuard(
            [&] { QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center()); });
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(2).center());
        dragTab(tabs, tabs->tabRect(1).center());
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(1).center());
        QCoreApplication::processEvents(); // submit the first order; its save will fail
        QVERIFY(service.busy());
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(1).center());
        dragTab(tabs, tabs->tabRect(0).center());
        auto expected = original;
        expected.move(2, 0);
        QCOMPARE(tabIds(tabs), expected);
        QTRY_COMPARE(failures.size(), 1);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(tabIds(tabs), expected); // old failure must not interrupt a new drag
        QCOMPARE(categoryIds(service.catalog()), original);
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
        QTRY_COMPARE(categoryIds(service.catalog()), expected);
        failNext = true;
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
        dragTab(tabs, tabs->tabRect(1).center());
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(1).center());
        QTRY_COMPARE(failures.size(), 2);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(tabIds(tabs), expected); // latest failure rolls back to committed order
        auto persisted = LauncherCatalog::load(path);
        QVERIFY(persisted);
        QCOMPARE(categoryIds(*persisted), expected);
        // A context-menu move resolves its UUID after earlier queued permutations.
        auto reversed = expected;
        std::reverse(reversed.begin(), reversed.end());
        service.reorderCategories(reversed);
        service.reorderCategory(reversed[0], 1);
        reversed.move(0, 1);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(categoryIds(service.catalog()), reversed);
        QCOMPARE(tabIds(tabs), reversed);
    }
    void backToBackCategoryDragsWaitForLatestRelease() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        QTest::qWait(30);
        auto* tabs = panel.findChild<QTabBar*>();
        const auto original = tabIds(tabs);
        const auto release = qScopeGuard(
            [&] { QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center()); });
        // Queue release processing, then start another drag before that callback runs.
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(2).center());
        dragTab(tabs, tabs->tabRect(1).center());
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(1).center());
        QTest::mousePress(tabs, Qt::LeftButton, {}, tabs->tabRect(1).center());
        dragTab(tabs, tabs->tabRect(0).center());
        auto expected = original;
        expected.move(2, 0);
        QTest::qWait(100);
        QVERIFY(!service.busy());
        QCOMPARE(categoryIds(service.catalog()), original);
        QCOMPARE(tabIds(tabs), expected);
        QTest::mouseRelease(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
        QTRY_COMPARE(categoryIds(service.catalog()), expected);
    }
    void refreshPreservesCategoryScroll() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        for (int i = 0; i < 12; ++i)
            catalog.categories.append(
                {LauncherCatalog::newId(), QStringLiteral("Category %1").arg(i), {}});
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        auto* tabs = panel.findChild<QTabBar*>();
        auto* right = tabs->findChild<QToolButton*>(QStringLiteral("ScrollRightButton"));
        QVERIFY(right);
        QTRY_VERIFY(right->isEnabled());
        for (int i = 0; i < 4 && right->isEnabled(); ++i)
            right->click();
        const auto lastRect = tabs->tabRect(tabs->count() - 1);
        panel.refresh();
        QCOMPARE(tabs->tabRect(tabs->count() - 1), lastRect);
    }
    void insertAddsAfterOccupiedSlotWhileF2Edits() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        const auto original = item(QStringLiteral("original"));
        QVERIFY(catalog.put(2, 0, original));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        activateForTest(&panel);
        tile(panel, 0)->setFocus();
        QString title;
        QTimer::singleShot(0, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>();
            if (!dialog)
                return;
            title = dialog->windowTitle();
            if (title != tx(u"添加启动项")) {
                dialog->reject();
                return;
            }
            const auto edits = dialog->findChildren<QLineEdit*>();
            edits[0]->setText(QStringLiteral("added"));
            edits[1]->setText(QStringLiteral("added.exe"));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        key(QApplication::focusWidget(), Qt::Key_Insert);
        QCOMPARE(title, tx(u"添加启动项"));
        QTRY_VERIFY(!service.busy());
        QCOMPARE(service.catalog().categories[2].slots[0], original.id);
        QVERIFY(!service.catalog().categories[2].slots[1].isEmpty());
        tile(panel, 0)->setFocus();
        QTimer::singleShot(0, &panel, [&] {
            if (auto* dialog = panel.findChild<QDialog*>()) {
                title = dialog->windowTitle();
                dialog->reject();
            }
        });
        key(QApplication::focusWidget(), Qt::Key_F2);
        QCOMPARE(title, tx(u"编辑启动项"));
    }
    void keypadEnterLaunchesFromFocusedGridAndSearch() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        const auto entry = item(QStringLiteral("keypad"));
        QVERIFY(catalog.put(2, 0, entry));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        activateForTest(&panel);
        tile(panel, 0)->setFocus();
        QSignalSpy launches(&panel, &LauncherPanel::launchRequested);
        key(QApplication::focusWidget(), Qt::Key_Enter, Qt::KeypadModifier);
        QCOMPARE(launches.size(), 1);
        for (bool focusResults : {false, true}) {
            key(tile(panel, 0), Qt::Key_F, Qt::ControlModifier);
            auto* search = panel.findChild<QLineEdit*>();
            search->setText(QStringLiteral("keypad"));
            activateForTest(search->window());
            if (focusResults)
                panel.findChild<QListWidget*>()->setFocus();
            else
                search->setFocus();
            key(QApplication::focusWidget(), Qt::Key_Enter, Qt::KeypadModifier);
        }
        QCOMPARE(launches.size(), 3);
        for (const auto& launch : launches)
            QCOMPARE(qvariant_cast<LaunchItem>(launch[0]).id, entry.id);
    }
    void clippedTilesCannotReceiveDrops() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.setGrid(20, 20);
        panel.showNear({20, 20});
        panel.fitToWorkArea(QRect(0, 0, 600, 500));
        QTest::qWait(30);
        auto* scroll = panel.findChild<QScrollArea*>();
        scroll->verticalScrollBar()->setValue(100);
        QSignalSpy imports(&panel, &LauncherPanel::importRequested);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/fixture.txt"))});
        const auto dropAt = [&](QPoint p) {
            QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
            QApplication::sendEvent(&panel, &enter);
            QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
            QApplication::sendEvent(&panel, &drop);
            return drop.isAccepted();
        };
        auto* tabs = panel.findChild<QTabBar*>();
        QVERIFY(!dropAt(tabs->mapTo(&panel, tabs->rect().center())));
        QVERIFY(!dropAt(QPoint(40, 15))); // clipped cells also overlap the header
        QCOMPARE(imports.size(), 0);
        auto* target = tile(panel, 40);
        const auto point = target->mapTo(&panel, target->rect().center());
        QVERIFY(scroll->viewport()->rect().contains(scroll->viewport()->mapFrom(&panel, point)));
        QVERIFY(dropAt(point));
        QCOMPARE(imports.size(), 1);
        QCOMPARE(imports[0][2].toInt(), 40);
    }
    void ordinaryRefreshDoesNotRepolishGrid() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        LauncherPanel panel(theme, service);
        panel.setGrid(20, 20);
        panel.setPinned(true);
        panel.showNear({20, 20});
        QTest::qWait(30);
        StyleChanges changes;
        for (int i = 0; i < 400; ++i)
            tile(panel, i)->installEventFilter(&changes);
        const auto dark = tile(panel, 0)->styleSheet();
        panel.refresh();
        QCOMPARE(changes.count, 0);
        theme.apply(QStringLiteral("light"));
        QVERIFY(tile(panel, 0)->styleSheet() != dark);
        QVERIFY(changes.count > 0);
    }
    void panelTitleUsesProductName() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("PanelTitle"))->text(),
                 QStringLiteral("MT"));
    }
    void lastPageCannotAddressInvalidSlots() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.setGrid(3, 3);
        panel.showNear({20, 20});
        activateForTest(&panel);
        tile(panel, 0)->setFocus();
        for (int i = 0; i < 1115; ++i)
            key(QApplication::focusWidget(), Qt::Key_PageDown);
        QCOMPARE(panel.windowTitle(), tx(u"MT · 启动面板 · 第 %1 页").arg(1112));
        QVERIFY(tile(panel, 0)->isEnabled());
        for (int i = 1; i < 9; ++i)
            QVERIFY(!tile(panel, i)->isEnabled());
        key(QApplication::focusWidget(), Qt::Key_Right);
        QCOMPARE(QApplication::focusWidget(), tile(panel, 0));
        bool nextEnabled = true;
        QTimer::singleShot(0, &panel, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (auto* action : menu->actions())
                    if (action->text() == tx(u"下一页（PageDown）"))
                        nextEnabled = action->isEnabled();
                menu->close();
            }
        });
        for (auto* button : panel.findChildren<QToolButton*>())
            if (button->accessibleName() == tx(u"启动器菜单")) {
                button->click();
                break;
            }
        QVERIFY(!nextEnabled);
        auto catalog = LauncherCatalog::empty();
        QVERIFY(catalog.put(2, 9999, item(QStringLiteral("last"))));
        QVERIFY(catalog.validate());
    }
    void finalSlotPersistsAndFullInsertFailsWithoutMutation() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        auto catalog = LauncherCatalog::empty();
        const auto last = item(QStringLiteral("last"));
        QVERIFY(catalog.put(2, 9999, last));
        QCOMPARE(catalog.categories[2].slots.size(), 10000);
        QVERIFY(catalog.save(path));
        const auto loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->categories[2].slots[9999], last.id);
        const auto before = catalog.encode();
        QVERIFY(!catalog.insertBlank(2, 9999));
        QCOMPARE(catalog.encode(), before);
        LauncherService service(path);
        QVERIFY(service.load());
        bool completed = false, succeeded = true;
        QSignalSpy changed(&service, &LauncherService::changed);
        service.insertBlank(catalog.selectedCategory, 9999, [&](auto result) {
            completed = true;
            succeeded = result.has_value();
        });
        QTRY_VERIFY(completed);
        QVERIFY(!succeeded);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(service.catalog().encode(), before);
        QCOMPARE(bytes(path), before);
    }
    void existingItemsRequestIconsAfterConnections() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        QVERIFY(catalog.put(2, 0, item(QStringLiteral("app A"))));
        QVERIFY(catalog.put(2, 1, item(QStringLiteral("app B"))));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        QSignalSpy requests(&panel, &LauncherPanel::iconRequested);
        panel.refresh();
        QTRY_COMPARE(requests.size(), 2);
        panel.refresh();
        QCOMPARE(requests.size(), 2); // pending requests remain deduplicated
    }
    void iconCompletionPreservesSearchSelectionAndLaunch() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        auto a = item(QStringLiteral("app A")), b = item(QStringLiteral("app B"));
        QVERIFY(catalog.put(2, 0, a));
        QVERIFY(catalog.put(2, 1, b));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        QSignalSpy requests(&panel, &LauncherPanel::iconRequested);
        panel.showNear({20, 20});
        QTRY_COMPARE(requests.size(), 2);
        key(&panel, Qt::Key_F, Qt::ControlModifier);
        auto* search = panel.findChild<QLineEdit*>();
        auto* results = panel.findChild<QListWidget*>();
        QVERIFY(search && results);
        search->setText(QStringLiteral("app"));
        QCOMPARE(results->count(), 2);
        results->setCurrentRow(1);
        const auto selected = results->currentItem()->data(Qt::UserRole).toString();
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::red);
        panel.acceptIcon(a.id, a.target, image);
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toString(), selected);
        panel.refresh();
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toString(), selected);
        QSignalSpy launched(&panel, &LauncherPanel::launchRequested);
        key(search, Qt::Key_Return);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(qvariant_cast<LaunchItem>(launched[0][0]).id, selected);
    }
    void dragSwitchesCategoryAndMovesToItsSlot() {
        QTemporaryDir dir;
        auto catalog = LauncherCatalog::empty();
        auto a = item(QStringLiteral("drag"));
        QVERIFY(catalog.put(2, 0, a));
        const auto destination = catalog.categories[1].id;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(catalog.save(path));
        LauncherService service(path);
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        QTest::qWait(30);
        auto* tabs = panel.findChild<QTabBar*>();
        QVERIFY(tabs);
        const auto position = tabs->mapTo(&panel, tabs->tabRect(1).center());
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-mt-launch-item"), a.id.toUtf8());
        QDragEnterEvent enter(position, Qt::MoveAction, &mime, Qt::LeftButton, {});
        QApplication::sendEvent(&panel, &enter);
        QDragMoveEvent move(position, Qt::MoveAction, &mime, Qt::LeftButton, {});
        QApplication::sendEvent(&panel, &move);
        // Drop immediately, before the asynchronous selection save has completed.
        auto* target = tile(panel, 3);
        QVERIFY(target);
        QDropEvent drop(target->mapTo(&panel, target->rect().center()), Qt::MoveAction, &mime,
                        Qt::LeftButton, {});
        QApplication::sendEvent(&panel, &drop);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(service.catalog().selectedCategory, destination);
        QCOMPARE(service.catalog().categories[1].slots[3], a.id);
        QVERIFY(service.catalog().categories[2].slots[0].isEmpty());
    }
    void searchGroupHidesOnlyAfterFocusLeaves() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.showNear({20, 20});
        activateForTest(&panel);
        key(&panel, Qt::Key_F, Qt::ControlModifier);
        auto* popup = panel.findChild<QLineEdit*>()->window();
        QVERIFY(popup->isVisible());
        QDialog owned(&panel);
        owned.show();
        activateForTest(&owned);
        QEvent loss(QEvent::WindowDeactivate);
        QApplication::sendEvent(popup, &loss);
        QTest::qWait(150);
        QVERIFY(panel.isVisible());
        owned.hide();
        QWidget external;
        external.show();
        activateForTest(&external);
        QApplication::sendEvent(popup, &loss);
        QTRY_VERIFY(!panel.isVisible());
        QVERIFY(!popup->isVisible());
        panel.setPinned(true);
        panel.showNear({20, 20});
        key(&panel, Qt::Key_F, Qt::ControlModifier);
        activateForTest(&external);
        QApplication::sendEvent(popup, &loss);
        QTest::qWait(150);
        QVERIFY(panel.isVisible());
    }
    void visiblePinnedPanelRetainsChineseLabels() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.showNear({20, 20});
        panel.retranslate();
        QCOMPARE(panel.findChild<QLineEdit*>()->placeholderText(), tx(u"搜索所有分类"));
        bool translated = false;
        for (auto* button : panel.findChildren<QToolButton*>())
            translated |= button->accessibleName() == tx(u"保持打开（Ctrl+Space）");
        QVERIFY(translated);
        QVERIFY(panel.isVisible());
        panel.hide();
        panel.showNear({20, 20});
        QCOMPARE(panel.findChild<QLineEdit*>()->placeholderText(), tx(u"搜索所有分类"));
    }
    void oversizedGridKeepsCategoriesReachable() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        Theme theme;
        LauncherPanel panel(theme, service);
        panel.setPinned(true);
        panel.setGrid(20, 20);
        panel.showNear({20, 20});
        key(&panel, Qt::Key_F, Qt::ControlModifier);
        auto* searchPopup = panel.findChild<QLineEdit*>()->window();
        QVERIFY(panel.screen()->availableGeometry().contains(searchPopup->geometry()));
        panel.fitToWorkArea(QRect(0, 0, 600, 500));
        QTest::qWait(30);
        QCOMPARE(panel.size(), QSize(600, 500));
        QCOMPARE(panel.gridRows(), 20);
        QCOMPARE(panel.gridColumns(), 20);
        auto* tabs = panel.findChild<QTabBar*>();
        QVERIFY(tabs);
        QVERIFY(panel.rect().contains(QRect(tabs->mapTo(&panel, QPoint{}), tabs->size())));
        auto* scroll = panel.findChild<QScrollArea*>();
        QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QVERIFY(scroll->horizontalScrollBar()->maximum() > 0);
        scroll->ensureWidgetVisible(tile(panel, 399));
        QVERIFY(scroll->verticalScrollBar()->value() > 0);
        QVERIFY(scroll->horizontalScrollBar()->value() > 0);
    }
    void completeHLaunchImportIsIdempotent() {
        QTemporaryDir source, destination;
        QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("data/shortcuts"))));
        const auto shortcut = source.filePath(QStringLiteral("data/shortcuts/special.lnk"));
        QVERIFY(write(shortcut, "fixture shortcut bytes"));
        QJsonObject entry{{QStringLiteral("id"), QStringLiteral("source-id")},
                          {QStringLiteral("name"), QStringLiteral("special")},
                          {QStringLiteral("type"), QStringLiteral("shortcut")},
                          {QStringLiteral("target"), shortcut},
                          {QStringLiteral("gridSlot"), 0}};
        const QJsonObject root{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("tabs"),
             QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("import")},
                                    {QStringLiteral("items"), QJsonArray{entry}}}}}};
        QVERIFY(write(source.filePath(QStringLiteral("data/items.json")),
                      QJsonDocument(root).toJson()));
        auto catalog = LauncherCatalog::empty();
        auto first = readHLaunchData(source.path(), destination.path(), 4);
        QVERIFY(first);
        QCOMPARE(catalog.merge(first->categories), 1);
        auto second = readHLaunchData(source.path(), destination.path(), 4);
        QVERIFY(second);
        QCOMPARE(second->skipped, 0);
        int duplicates = 0;
        QCOMPARE(catalog.merge(second->categories, &duplicates), 0);
        QCOMPARE(duplicates, 1);
        QCOMPARE(QDir(destination.filePath(QStringLiteral("shortcuts")))
                     .entryList({QStringLiteral("*.lnk")}, QDir::Files | QDir::System)
                     .size(),
                 1);
        QCOMPARE(bytes(shortcut), QByteArray("fixture shortcut bytes"));
    }
    void urlDeduplicationPreservesPathAndQueryCase() {
        auto c = LauncherCatalog::empty();
        auto a = item(QStringLiteral("url"));
        a.target = QStringLiteral("https://example.com/Foo?token=Ab");
        QVERIFY(c.put(0, 0, a));
        auto b = a;
        b.id = LauncherCatalog::newId();
        b.target = QStringLiteral("https://example.com/foo?token=Ab");
        QVERIFY(c.duplicateOf(b).isEmpty());
        b.target = QStringLiteral("https://example.com/Foo?token=ab");
        QVERIFY(c.duplicateOf(b).isEmpty());
        b.target = QStringLiteral("https://EXAMPLE.COM/Foo?token=Ab");
        QCOMPARE(c.duplicateOf(b), a.id);
    }
    void failedConfigurationLeavesBothSnapshotsUnchanged() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        auto c = LauncherCatalog::empty();
        QVERIFY(c.setColumns(3));
        QVERIFY(c.put(2, 2000, item(QStringLiteral("high"))));
        QVERIFY(c.save(path));
        SettingsStore store(dir.path());
        auto settings = Settings::defaults();
        settings.launcherColumns = 3;
        QVERIFY(store.save(settings));
        const auto catalogBefore = bytes(path),
                   settingsBefore = bytes(dir.filePath(QStringLiteral("settings.json")));
        LauncherService service(path);
        QVERIFY(service.load());
        settings.launcherColumns = 20;
        bool completed = false, succeeded = true;
        service.configure(store, settings, [&](auto result) {
            completed = true;
            succeeded = result.has_value();
        });
        QTRY_VERIFY(completed);
        QVERIFY(!succeeded);
        QCOMPARE(service.catalog().columns, 3);
        QCOMPARE(bytes(path), catalogBefore);
        QCOMPARE(bytes(dir.filePath(QStringLiteral("settings.json"))), settingsBefore);
        // A settings destination that cannot be replaced must not change launcher.json either.
        QVERIFY(QFile::remove(dir.filePath(QStringLiteral("settings.json"))));
        QVERIFY(QDir().mkdir(dir.filePath(QStringLiteral("settings.json"))));
        settings.launcherColumns = 4;
        completed = false;
        service.configure(store, settings, [&](auto result) {
            completed = true;
            succeeded = result.has_value();
        });
        QTRY_VERIFY(completed);
        QVERIFY(!succeeded);
        QCOMPARE(bytes(path), catalogBefore);
        QCOMPARE(service.catalog().columns, 3);
    }
    void interruptedConfigurationRecoversBeforeLoading() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        auto settings = Settings::defaults();
        QVERIFY(store.save(settings));
        auto c = LauncherCatalog::empty();
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(c.save(path));
        QJsonObject journal{{QStringLiteral("schema"), 1}};
        for (const auto& name : {QStringLiteral("launcher.json"), QStringLiteral("settings.json")})
            journal[name] =
                QJsonObject{{QStringLiteral("existed"), true},
                            {QStringLiteral("bytes"),
                             QString::fromLatin1(bytes(dir.filePath(name)).toBase64())}};
        QVERIFY(write(dir.filePath(QStringLiteral("launcher-settings-rollback.json")),
                      QJsonDocument(journal).toJson()));
        QVERIFY(c.setColumns(6));
        QVERIFY(c.save(path));
        QVERIFY(write(dir.filePath(QStringLiteral("settings.json")), "interrupted"));
        auto loaded = store.load();
        QVERIFY(loaded);
        QCOMPARE(loaded->launcherColumns, 4);
        auto catalog = LauncherCatalog::load(path);
        QVERIFY(catalog);
        QCOMPARE(catalog->columns, 4);
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("launcher-settings-rollback.json"))));
    }
    void successfulConfigurationIncludesEarlierQueuedEdits() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        SettingsStore store(dir.path());
        auto settings = Settings::defaults();
        QVERIFY(store.save(settings));
        LauncherService service(path);
        QVERIFY(service.load());
        const auto entry = item(QStringLiteral("keep row and column"));
        QVERIFY(service.put(service.catalog().selectedCategory, 5, entry));
        settings.launcherColumns = 6;
        settings.launcherRows = 3;
        settings.theme = QStringLiteral("light");
        bool completed = false, success = false;
        service.configure(store, settings, [&](auto result) {
            completed = true;
            success = result.has_value();
        });
        QTRY_VERIFY(completed);
        QVERIFY(success);
        QCOMPARE(service.catalog().columns, 6);
        QCOMPARE(service.catalog().categories[2].slots[7], entry.id);
        auto disk = LauncherCatalog::load(path);
        auto config = store.load();
        QVERIFY(disk && config);
        QCOMPARE(disk->columns, config->launcherColumns);
        QCOMPARE(config->theme, QStringLiteral("light"));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("launcher-settings-rollback.json"))));
    }
    void serialSavesKeepGuiResponsiveAndPublishOnlyOnSuccess() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        std::atomic<bool> workerUsed = false;
        std::atomic<int> writes = 0;
        LauncherService service(path, nullptr, [&](const auto& c, const auto& p) {
            workerUsed = QThread::currentThread() != qApp->thread();
            ++writes;
            QThread::msleep(80);
            return c.save(p);
        });
        QVERIFY(service.load());
        auto a = item(QStringLiteral("a"));
        const auto category = service.catalog().selectedCategory;
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(5);
        QVERIFY(service.put(category, 0, a));
        QVERIFY(service.catalog().items.isEmpty());
        QVERIFY(service.recordLaunch(a.id));
        QVERIFY(service.recordLaunch(a.id));
        QTRY_VERIFY(!service.busy());
        QVERIFY(workerUsed.load());
        QVERIFY(ticks > 5);
        QCOMPARE(writes.load(), 3);
        QCOMPARE(service.catalog().items[a.id].launchCount, 2);
        auto disk = LauncherCatalog::load(path);
        QVERIFY(disk);
        QCOMPARE(disk->items[a.id].launchCount, 2);
    }
    void shutdownDrainsAcceptedWrites() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        auto a = item(QStringLiteral("a"));
        {
            LauncherService service(path);
            QVERIFY(service.load());
            QVERIFY(service.put(service.catalog().selectedCategory, 0, a));
            QVERIFY(service.recordLaunch(a.id));
            service.shutdown(); // does not pump GUI events to complete the writes
        }
        const auto disk = LauncherCatalog::load(path);
        QVERIFY(disk);
        QCOMPARE(disk->items[a.id].launchCount, 1);
    }
    void settingsDialogWaitsForPersistenceCompletion() {
        Theme theme;
        SettingsDialog::Completion completion;
        SettingsDialog dialog(theme, Settings::defaults(), QStringLiteral("fixture"),
                              [&](const Settings&, SettingsDialog::Completion done) {
                                  completion = std::move(done);
                              });
        dialog.show();
        dialog.findChild<QSpinBox*>(QStringLiteral("launcherColumns"))->setValue(5);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(completion);
        QVERIFY(!dialog.isEnabled());
        dialog.reject();
        QVERIFY(dialog.isVisible());
        completion({});
        QVERIFY(dialog.isEnabled() && dialog.isVisible());
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!dialog.isVisible());
    }
    void shellIconIdentityAndTransientFailures() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("icon.png"));
        ShellJobs jobs;
        QImage received;
        int completed = 0;
        auto request = [&](const QString& id) {
            jobs.icon(id, path, dir.path(), [&](QImage image) {
                received = image;
                ++completed;
            });
        };
        request(QStringLiteral("first"));
        QTRY_COMPARE(completed, 1);
        QVERIFY(received.isNull());
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(path));
        request(QStringLiteral("first"));
        QTRY_COMPARE(completed, 2);
        QCOMPARE(received.pixelColor(0, 0), QColor(Qt::red));
        image.fill(Qt::blue);
        QVERIFY(image.save(path));
        request(QStringLiteral("replacement-id"));
        QTRY_COMPARE(completed, 3);
        QCOMPARE(received.pixelColor(0, 0), QColor(Qt::blue));
    }
    void revealUsesHelperWithoutBlockingGuiAndReportsErrors() {
        ShellJobs jobs;
        bool completed = false, success = false;
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(5);
        jobs.reveal(QStringLiteral("slow"), QString{}, [&](auto result) {
            success = result.has_value();
            completed = true;
        });
        QVERIFY(!completed);
        QTRY_VERIFY(completed);
        QVERIFY(success);
        QVERIFY(ticks > 5);
        completed = false;
        jobs.reveal(QStringLiteral("fail"), QString{}, [&](auto result) {
            success = result.has_value();
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(!success);
        bool callbackAfterShutdown = false;
        jobs.reveal(QStringLiteral("slow"), QString{}, [&](auto) { callbackAfterShutdown = true; });
        jobs.shutdown();
        QTest::qWait(30);
        QVERIFY(!callbackAfterShutdown);
    }
};
int main(int argc, char** argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--shell-helper") {
        QCoreApplication app(argc, argv);
        return helper();
    }
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setQuitOnLastWindowClosed(false);
    LauncherRegressionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "LauncherRegressionTests.moc"
