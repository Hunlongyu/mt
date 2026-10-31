#include <windows.h>
#include "app/Application.h"
#include "modules/clipboard/ClipboardService.h"
#include "modules/clipboard/ClipboardPanel.h"
#include "modules/clipboard/ClipboardThumbnailCache.h"
#include "ui/shell/SettingsDialog.h"
#include "platform/windows/CaretLocator.h"
#include "infrastructure/storage/ClipboardStore.h"
#include "infrastructure/storage/HopyImport.h"
#include "infrastructure/i18n/I18n.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTabWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QUrl>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtPlugin>
#include <QtTest>
#include <atomic>
#include <thread>
#include <stdexcept>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace mt {
class ClipboardIntegrationTests final : public QObject {
    Q_OBJECT
    struct CaptureSource {
        const QMimeData* mime = nullptr;
        quint32 sequence = 100;
        int reads = 0;
    };
    static void bindCaptureSource(ClipboardService& service, CaptureSource& source) {
        service.observing_ = true; // No connection to the actual system clipboard.
        service.clipboardMimeData_ = [&source] {
            ++source.reads;
            return source.mime;
        };
        service.clipboardSequence_ = [&source] { return source.sequence; };
    }
    static ClipboardRecords lastRecords(const QSignalSpy& records) {
        return records.isEmpty() ? ClipboardRecords{}
                                 : qvariant_cast<ClipboardRecords>(records.last().at(0));
    }
  private Q_SLOTS:
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
    }
    void delayedImageCaptureRetriesWithoutReportingOrBlocking() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        QMimeData unavailable, available;
        unavailable.setImageData(QImage{});
        QImage bitmap(12, 9, QImage::Format_ARGB32);
        bitmap.fill(Qt::green);
        available.setImageData(bitmap);
        CaptureSource source{&unavailable};
        bindCaptureSource(service, source);
        service.snapshot();
        QCOMPARE(source.reads, 1);
        QCOMPARE(errors.size(), 0);
        bool guiTicked = false;
        QTimer::singleShot(0, &service, [&] { guiTicked = true; source.mime = &available; });
        QTRY_COMPARE(lastRecords(records).size(), 1);
        QVERIFY(guiTicked);
        QCOMPARE(source.reads, 2);
        QCOMPARE(lastRecords(records).front().kind, ClipKind::Image);
        bool loaded = false;
        service.retrieve(lastRecords(records).front().id, [&](ClipboardRecord record) {
            QCOMPARE(QImage::fromData(record.image), bitmap);
            loaded = true;
        });
        QTRY_VERIFY(loaded);
        QTest::qWait(850);
        QCOMPARE(source.reads, 2);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(lastRecords(records).size(), 1);
    }
    void unreadableImageCaptureEventuallyReportsOnce() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        ClipboardRecord existing;
        existing.text = QStringLiteral("preserve existing history");
        service.capture(existing);
        QTRY_COMPARE(lastRecords(records).size(), 1);
        QMimeData unavailable;
        unavailable.setImageData(QImage{});
        CaptureSource source{&unavailable};
        bindCaptureSource(service, source);
        service.snapshot();
        QCOMPARE(errors.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 2500);
        QCOMPARE(source.reads, 5);
        QTest::qWait(500);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(source.reads, 5);
        QCOMPARE(lastRecords(records).front().text, existing.text);
    }
    void newCopySupersedesPendingImageCapture() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        QMimeData unavailable, newer;
        unavailable.setImageData(QImage{});
        newer.setText(QStringLiteral("new copy"));
        CaptureSource source{&unavailable};
        bindCaptureSource(service, source);
        service.snapshot();
        source.mime = &newer;
        ++source.sequence;
        service.snapshot();
        QTRY_COMPARE(lastRecords(records).size(), 1);
        QTest::qWait(100);
        QCOMPARE(lastRecords(records).front().text, newer.text());
        QCOMPARE(source.reads, 2);
        QCOMPARE(errors.size(), 0);
    }
    void pendingImageCaptureCancelsOnSequenceChangePauseDisableAndWriteback_data() {
        QTest::addColumn<int>("action");
        QTest::newRow("sequence-before-notification") << 0;
        QTest::newRow("pause-and-resume") << 1;
        QTest::newRow("disable-and-enable") << 2;
        QTest::newRow("own-writeback-notification") << 3;
    }
    void pendingImageCaptureCancelsOnSequenceChangePauseDisableAndWriteback() {
        QFETCH(int, action);
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        QMimeData unavailable, newer;
        unavailable.setImageData(QImage{});
        newer.setText(QStringLiteral("must not capture stale attempt"));
        CaptureSource source{&unavailable};
        bindCaptureSource(service, source);
        service.snapshot();
        source.mime = &newer;
        if (action == 0) ++source.sequence;
        if (action == 1) {
            service.configure(true, true, 1000, 500);
            service.configure(true, false, 1000, 500);
        }
        if (action == 2) {
            service.configure(false, false, 1000, 500);
            service.configure(true, false, 1000, 500);
        }
        if (action == 3) {
            newer.setData(QStringLiteral("application/x-mt-writeback"), service.ownMarker_.toUtf8());
            ++source.sequence;
            service.snapshot();
        }
        QTest::qWait(150);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(source.reads, action == 3 ? 2 : 1);
        QVERIFY(lastRecords(records).isEmpty());
    }
    void destroyedServiceCancelsPendingImageCapture() {
        QTemporaryDir dir;
        QMimeData unavailable;
        unavailable.setImageData(QImage{});
        CaptureSource source{&unavailable};
        int errors = 0;
        {
            ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
            bindCaptureSource(service, source);
            connect(&service, &ClipboardService::failed, this, [&](const QString&) { ++errors; });
            service.snapshot();
        }
        QTest::qWait(150);
        QCOMPARE(source.reads, 1);
        QCOMPARE(errors, 0);
    }
    void clipboardChangeDuringReadDiscardsTheResult() {
        class ChangingImageMime final : public QMimeData {
          public:
            std::function<void()> duringRead;
          protected:
            QVariant retrieveData(const QString& name, QMetaType type) const override {
                auto result = QMimeData::retrieveData(name, type);
                if (name == QStringLiteral("application/x-qt-image"))
                    duringRead();
                return result;
            }
        };
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        ChangingImageMime older;
        older.setImageData(QImage(4, 4, QImage::Format_RGB32));
        CaptureSource source{&older};
        bindCaptureSource(service, source);
        older.duringRead = [&] {
            ++source.sequence; // Windows sequence changes before its notification is dispatched.
        };
        service.snapshot();
        QTest::qWait(100);
        QVERIFY(lastRecords(records).isEmpty());
        QCOMPARE(source.reads, 1);
        QCOMPARE(errors.size(), 0);
    }
    void missingMimeDataCanRecoverOnTheNextAttempt() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy records(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        QMimeData available;
        available.setImageData(QImage(4, 4, QImage::Format_RGB32));
        CaptureSource source;
        bindCaptureSource(service, source);
        service.snapshot();
        source.mime = &available;
        QTRY_COMPARE(lastRecords(records).size(), 1);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(source.reads, 2);
    }
    void invalidImagePayloadDoesNotRetry() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy errors(&service, &ClipboardService::failed);
        QMimeData oversized;
        oversized.setImageData(QImage(ClipboardRecord::maxImageSide + 1, 1, QImage::Format_ARGB32));
        CaptureSource source{&oversized};
        bindCaptureSource(service, source);
        service.snapshot();
        QCOMPARE(errors.size(), 1);
        QTest::qWait(100);
        QCOMPARE(source.reads, 1);
        QCOMPARE(errors.size(), 1);
    }
    void unsupportedClipboardContentStaysSilent() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy errors(&service, &ClipboardService::failed);
        QMimeData empty, invisibleHtml;
        invisibleHtml.setHtml(QStringLiteral("<!-- no visible content -->"));
        CaptureSource source{&empty};
        bindCaptureSource(service, source);
        service.snapshot();
        source.mime = &invisibleHtml;
        ++source.sequence;
        service.snapshot();
        QTest::qWait(100);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(source.reads, 2);
    }
    void hostCanExitWithSystemSettingsOpen() {
        QTemporaryDir dir;
        DesktopServices desktop(dir.path());
        auto app = std::make_unique<Application>(dir.path(), true, desktop);
        QVERIFY(app->start());
        app->showSettings();
        QPointer<SettingsDialog> settings = app->settingsDialog_;
        QVERIFY(settings);
        QVERIFY(!settings->findChild<QWidget*>(QStringLiteral("autoStart"))->isEnabled());
        QVERIFY(!settings->findChild<QWidget*>(QStringLiteral("adminSupport"))->isEnabled());
        app.reset();
        QVERIFY(settings.isNull());
    }
    void trayAboutReusesSettingsAndPreservesDraft() {
        const auto previousName = qApp->applicationDisplayName();
        const auto restoreName = qScopeGuard([&] { qApp->setApplicationDisplayName(previousName); });
        qApp->setApplicationDisplayName(QStringLiteral("MT"));
        QTemporaryDir dir;
        DesktopServices desktop(dir.path());
        Application app(dir.path(), true, desktop);
        QVERIFY(app.start());
        QAction* about = nullptr;
        for (auto* action : app.menu_->actions())
            if (action->text() == QStringLiteral("关于"))
                about = action;
        QVERIFY(about);
        about->trigger();
        QPointer<SettingsDialog> settings = app.settingsDialog_;
        QVERIFY(settings);
        QVERIFY(settings->isVisible());
        auto* pages = settings->findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
        QVERIFY(pages);
        QCOMPARE(pages->currentIndex(), 5);
        wchar_t nativeTitle[256]{};
        QVERIFY(GetWindowTextW(reinterpret_cast<HWND>(settings->winId()), nativeTitle, 256) > 0);
        QCOMPARE(QString::fromWCharArray(nativeTitle), QStringLiteral("设置 - MT"));

        auto* rows = settings->findChild<QSpinBox*>(QStringLiteral("launcherRows"));
        QVERIFY(rows);
        const auto changedRows = rows->value() + 1;
        rows->setValue(changedRows);
        QVERIFY(settings->hasUnsavedChanges());
        for (auto* nav : settings->findChildren<QPushButton*>(QStringLiteral("SettingsNav")))
            if (nav->property("pageIndex").toInt() == 1)
                nav->click();
        QCOMPARE(pages->currentIndex(), 1);
        settings->showMinimized();
        about->trigger();
        QCOMPARE(app.settingsDialog_, settings);
        QVERIFY(!settings->isMinimized());
        QCOMPARE(pages->currentIndex(), 5);
        QCOMPARE(rows->value(), changedRows);
        QVERIFY(settings->hasUnsavedChanges());
        for (auto* nav : settings->findChildren<QPushButton*>(QStringLiteral("SettingsNav")))
            QCOMPARE(nav->isChecked(), nav->property("pageIndex").toInt() == 5);
        QVERIFY(settings->findChildren<QMessageBox*>().isEmpty());
    }
    void emptyHtmlFallsBackWithoutPollutingHistory() {
        QMimeData mime;
        mime.setHtml(QStringLiteral(""));
        QVERIFY(mime.hasHtml());
        QVERIFY(!ClipboardRecord::snapshot(mime));
        mime.setText(QStringLiteral("fallback"));
        auto text = ClipboardRecord::snapshot(mime);
        QVERIFY(text);
        QCOMPARE(text->kind, ClipKind::Text);
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(Qt::green);
        mime.setImageData(image);
        auto picture = ClipboardRecord::snapshot(mime);
        QVERIFY(picture);
        QCOMPARE(picture->kind, ClipKind::Image);

        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        QVERIFY(store.capture(*text, 1));
        ClipboardRecord empty;
        empty.kind = ClipKind::Html;
        QVERIFY(!store.capture(empty, 1));
        QCOMPARE(store.records()->front().text, QStringLiteral("fallback"));
    }
    void derivedHtmlTextCannotBypassLimit() {
        QMimeData mime;
        mime.setHtml(QStringLiteral("<p>") + QString(ClipboardRecord::maxText + 1, u'x') +
                     QStringLiteral("</p>"));
        QVERIFY(mime.html().size() < ClipboardRecord::maxHtml);
        QVERIFY(!ClipboardRecord::snapshot(mime));
    }
    void imageWithHtmlWithoutTextKeepsBitmap() {
        QImage bitmap(12, 9, QImage::Format_ARGB32);
        bitmap.fill(Qt::green);
        QMimeData mime;
        mime.setHtml(QStringLiteral("<img src='https://example.invalid/image.png'>"));
        mime.setImageData(bitmap);
        QVERIFY(!mime.hasText());
        auto record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Image);
        QCOMPARE(record->pendingImage, bitmap);
        record->finalize();
        QCOMPARE(qvariant_cast<QImage>(record->mime()->imageData()), bitmap);
        // A real text format still gives rich text priority over a bitmap.
        mime.setText(QStringLiteral("caption"));
        record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Html);
        QCOMPARE(record->text, QStringLiteral("caption"));
        // Preserve the existing HTML-only fallback when no bitmap is available.
        mime.clear();
        mime.setHtml(QStringLiteral("<b>only HTML</b>"));
        record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->text, QStringLiteral("only HTML"));
    }
    void imagePlainTextPasteKeepsBitmap() {
        ClipboardRecord record;
        record.kind = ClipKind::Image;
        record.pendingImage = QImage(12, 9, QImage::Format_ARGB32);
        record.pendingImage.fill(Qt::blue);
        record.finalize();
        const auto ordinary = record.mime();
        const auto plain = record.mime(true);
        QVERIFY(plain->hasImage());
        QVERIFY(!plain->hasText());
        QCOMPARE(qvariant_cast<QImage>(plain->imageData()),
                 qvariant_cast<QImage>(ordinary->imageData()));
    }
    void mixedUrlsKeepLocalFilesAndApplyLocalLimits() {
        QMimeData mime;
        const auto first = QUrl::fromLocalFile(QStringLiteral("C:/fixture/a.txt"));
        const auto last = QUrl::fromLocalFile(QStringLiteral("C:/fixture/b.txt"));
        const QUrl web(QStringLiteral("https://example.invalid/remote"));
        mime.setUrls({first, web, last});
        auto record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Files);
        QCOMPARE(record->files, QStringList({first.toLocalFile(), last.toLocalFile()}));
        QCOMPARE(record->mime()->urls(), QList<QUrl>({first, last}));
        QList<QUrl> urls(1001, web);
        urls.append(first);
        mime.setUrls(urls);
        record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->files.size(), 1);
        mime.setUrls(QList<QUrl>(1001, first));
        QVERIFY(!ClipboardRecord::snapshot(mime));
        mime.setUrls({web});
        record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Text);
    }
    void rapidFlagActionsPreserveIndependentState() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("clips.sqlite"));
        ClipboardRecord record;
        record.text = QStringLiteral("toggle fixture");
        record.finalize();
        {
            ClipboardStore store(path);
            QVERIFY(store.open());
            QVERIFY(store.capture(record, 10));
        }
        ClipboardService service(path);
        Theme theme;
        ClipboardPanel panel(theme, false);
        connect(&panel, &ClipboardPanel::toggleFlagRequested, &service, &ClipboardService::toggleFlag);
        connect(&service, &ClipboardService::recordsChanged, &panel, &ClipboardPanel::setRecords);
        QSignalSpy results(&service, &ClipboardService::recordsChanged);
        QSignalSpy errors(&service, &ClipboardService::failed);
        service.start(false);
        QTRY_VERIFY(!results.isEmpty());
        results.clear();
        // Deliberately do not process GUI events between actions: both see the same list.
        panel.performAction(0, 0);
        panel.performAction(0, 1);
        QTRY_VERIFY(!results.isEmpty());
        auto rows = results.last()[0].value<ClipboardRecords>();
        QVERIFY(rows.front().favorite);
        QVERIFY(rows.front().pinned);
        results.clear();
        panel.performAction(0, 0);
        panel.performAction(0, 0);
        panel.performAction(0, 1);
        panel.performAction(0, 1);
        QTRY_VERIFY(!results.isEmpty());
        rows = results.last()[0].value<ClipboardRecords>();
        QVERIFY(rows.front().favorite);
        QVERIFY(rows.front().pinned);
        QCOMPARE(errors.size(), 0);
    }
    void deletingSelectedRowKeepsVisibleNeighbor() {
        Theme theme;
        ClipboardPanel panel(theme, false);
        ClipboardRecords rows;
        for (int i = 0; i < 50; ++i) {
            ClipboardRecord record;
            record.id = record.text = QString::number(i);
            rows.append(record);
        }
        panel.setRecords(rows);
        panel.showNear({30, 30}, false);
        auto* list = panel.findChild<QListView*>();
        list->setCurrentIndex(list->model()->index(30, 0));
        list->scrollTo(list->currentIndex(), QAbstractItemView::PositionAtTop);
        rows.removeAt(30);
        panel.setRecords(rows);
        QCOMPARE(list->currentIndex().data(Qt::UserRole).value<ClipboardRecord>().id,
                 QStringLiteral("31"));
        QVERIFY(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())));
        list->setCurrentIndex(list->model()->index(48, 0));
        list->scrollTo(list->currentIndex());
        rows.removeLast();
        panel.setRecords(rows);
        QCOMPARE(list->currentIndex().row(), 47);
        QVERIFY(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())));
        // A record may also disappear while a search is open.
        panel.showSearch();
        panel.setRecords({rows.front()});
        rows.removeLast();
        panel.closeSearch();
        panel.setRecords(rows);
        QCOMPARE(list->currentIndex().row(), 46);
        panel.setRecords({});
        QVERIFY(!list->currentIndex().isValid());
    }
    void singleDeleteDoesNotOpenModalOrPaste() {
        Theme theme;
        ClipboardPanel panel(theme, false);
        ClipboardRecord record;
        record.id = record.text = QStringLiteral("delete fixture");
        panel.setRecords({record});
        panel.showNear({30, 30}, false);
        QSignalSpy removed(&panel, &ClipboardPanel::removeRequested);
        QSignalSpy pasted(&panel, &ClipboardPanel::pasteRequested);
        bool modalOpened = false;
        QTimer rescue;
        rescue.setInterval(10);
        connect(&rescue, &QTimer::timeout, &panel, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible()) {
                    modalOpened = true;
                    box->done(QMessageBox::No);
                }
        });
        rescue.start();
        panel.handleKey(Qt::Key_D);
        QVERIFY(!modalOpened);
        panel.handleKey(Qt::Key_Delete);
        auto* list = panel.findChild<QListView*>();
        const auto row = list->visualRect(list->model()->index(0, 0));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.right() - 29, row.top() + 22));
        QCOMPARE(removed.size(), 3);
        QCOMPARE(pasted.size(), 0);
        QVERIFY(panel.isVisible());
        // The context menu also returns immediately and uses the same delete path.
        Q_EMIT list->customContextMenuRequested(row.center());
        QPointer<QMenu> menu = panel.findChild<QMenu*>();
        QVERIFY(menu);
        menu->actions().last()->trigger();
        menu->close();
        QCOMPARE(removed.size(), 4);
        QTRY_VERIFY(menu.isNull());
    }
    void inlineHelpConsumesHistoryKeysAndClosesWithPanel() {
        Theme theme;
        ClipboardPanel panel(theme, false);
        ClipboardRecord record;
        record.id = record.text = QStringLiteral("help fixture");
        panel.setRecords({record});
        panel.showNear({30, 30}, false);
        QSignalSpy paste(&panel, &ClipboardPanel::pasteRequested);
        QSignalSpy flags(&panel, &ClipboardPanel::toggleFlagRequested);
        QSignalSpy remove(&panel, &ClipboardPanel::removeRequested);
        QSignalSpy preview(&panel, &ClipboardPanel::previewRequested);
        QSignalSpy open(&panel, &ClipboardPanel::openRequested);
        panel.handleKey(Qt::Key_H);
        auto* help = panel.findChild<QWidget*>(QStringLiteral("ClipboardHelp"));
        auto* list = panel.findChild<QListView*>();
        QVERIFY(help && help->isVisible());
        QVERIFY(!list->isVisible());
        QVERIFY(panel.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(!QApplication::activeModalWidget());
        for (const int key : {Qt::Key_F, Qt::Key_T, Qt::Key_D, Qt::Key_Delete,
                              Qt::Key_1, Qt::Key_Space, Qt::Key_O, Qt::Key_Up})
            panel.handleKey(key);
        panel.performAction(0, 2);
        QCOMPARE(flags.size(), 0);
        QCOMPARE(remove.size(), 0);
        QCOMPARE(preview.size(), 0);
        QCOMPARE(open.size(), 0);
        panel.handleKey(Qt::Key_Return);
        QCOMPARE(paste.size(), 0);
        QVERIFY(help->isHidden());
        QVERIFY(list->isVisible());
        panel.handleKey(Qt::Key_H);
        panel.handleKey(Qt::Key_Escape);
        QVERIFY(panel.isVisible());
        QVERIFY(help->isHidden());
        panel.handleKey(Qt::Key_H);
        panel.hide();
        QVERIFY(help->isHidden());
        panel.showNear({30, 30}, false);
        panel.handleKey(Qt::Key_Return);
        QCOMPARE(paste.size(), 1);
        // The toolbar menu reaches the same help mode without an exec() loop.
        QToolButton* more = nullptr;
        for (auto* button : panel.findChildren<QToolButton*>())
            if (button->accessibleName() == tx(u"更多操作")) more = button;
        QVERIFY(more);
        more->click();
        QPointer<QMenu> menu = panel.findChild<QMenu*>();
        QVERIFY(menu);
        menu->actions()[1]->trigger();
        menu->close();
        QVERIFY(help->isVisible());
        QTRY_VERIFY(menu.isNull());
    }
    void thumbnailCacheReusesPixelsAndBoundsMemory() {
        ClipboardRecord record;
        record.kind = ClipKind::Image;
        record.pendingImage = QImage(32, 24, QImage::Format_ARGB32);
        record.pendingImage.fill(Qt::red);
        record.finalize();
        record.image = record.thumbnail; // exactly what the list query returns
        ClipboardThumbnailCache cache(128);
        const auto first = cache.pixmap(record, {80, 60}, 1.0);
        QVERIFY(!first.isNull());
        record.favorite = true;
        cache.retain({record});
        QCOMPARE(cache.pixmap(record, {80, 60}, 1.0).cacheKey(), first.cacheKey());
        const auto dense = cache.pixmap(record, {80, 60}, 2.0);
        QCOMPARE(dense.size(), QSize(160, 120));
        QCOMPARE(dense.devicePixelRatio(), 2.0);
        QVERIFY(dense.cacheKey() != first.cacheKey());
        const auto resized = cache.pixmap(record, {40, 30}, 2.0);
        QCOMPARE(resized.size(), QSize(80, 60));
        QVERIFY(resized.cacheKey() != dense.cacheKey());
        ClipboardRecord replacement;
        replacement.kind = ClipKind::Image;
        replacement.pendingImage = QImage(32, 24, QImage::Format_ARGB32);
        replacement.pendingImage.fill(Qt::blue);
        replacement.finalize();
        record.image = replacement.thumbnail; // even a reused ID cannot keep stale pixels
        const auto updated = cache.pixmap(record, {40, 30}, 2.0);
        QCOMPARE(updated.toImage().pixelColor(0, 0), QColor(Qt::blue));
        for (int i = 0; i < 20; ++i) {
            record.id = QString::number(i);
            (void)cache.pixmap(record, {80, 60}, 1.0);
        }
        QVERIFY(cache.costKiB() <= cache.budgetKiB());
        cache.retain({});
        QCOMPARE(cache.costKiB(), 0);
    }
    void clearHistoryConfirmationIsAsyncAndDefaultsToNo() {
        Theme theme;
        SettingsDialog dialog(theme, Settings::defaults(),
            [](const Settings&, SettingsDialog::Completion done) { done({}); });
        dialog.findChild<QStackedWidget*>(QStringLiteral("settingsPages"))->setCurrentIndex(2);
        dialog.findChild<QTabWidget*>(QStringLiteral("clipboardTabs"))->setCurrentIndex(2);
        dialog.show();
        auto* button = dialog.findChild<QPushButton*>(QStringLiteral("clearClipboardHistory"));
        QVERIFY(button);
        QSignalSpy clear(&dialog, &SettingsDialog::clearHistoryRequested);
        button->click();
        QPointer<QMessageBox> confirm = dialog.findChild<QMessageBox*>(
            QStringLiteral("clearClipboardConfirmation"));
        QVERIFY(confirm && confirm->isVisible());
        QCOMPARE(clear.size(), 0);
        QCOMPARE(confirm->standardButton(confirm->defaultButton()), QMessageBox::No);
        QVERIFY(!button->isEnabled());
        confirm->button(QMessageBox::No)->click();
        QTRY_VERIFY(confirm.isNull());
        QCOMPARE(clear.size(), 0);
        QVERIFY(button->isEnabled());
        button->click();
        confirm = dialog.findChild<QMessageBox*>(QStringLiteral("clearClipboardConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(confirm.isNull());
        QCOMPARE(clear.size(), 1);
        button->click();
        confirm = dialog.findChild<QMessageBox*>(QStringLiteral("clearClipboardConfirmation"));
        QVERIFY(confirm);
        confirm->reject();
        QTRY_VERIFY(confirm.isNull());
        QCOMPARE(clear.size(), 1);
    }
    void failedStreamImportRollsBackRowsAndPruning() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord original;
        original.text = QStringLiteral("keep me");
        QVERIFY(store.capture(original, 1));
        auto produce = [](const ClipConsumer& consume) -> std::expected<void, QString> {
            ClipboardRecord r;
            r.text = QStringLiteral("must roll back");
            if (auto inserted = consume(r); !inserted)
                return inserted;
            return std::unexpected(QStringLiteral("synthetic read failure"));
        };
        QVERIFY(!store.importStream(produce, 1));
        QCOMPARE(store.records()->size(), 1);
        QCOMPARE(store.records()->front().text, original.text);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)store.importStream(
            [](const ClipConsumer& consume) -> std::expected<void, QString> {
                ClipboardRecord r;
                r.text = QStringLiteral("throw after insert");
                (void)consume(r);
                throw std::runtime_error("synthetic decoder failure");
            }, 1));
        QCOMPARE(store.records()->front().text, original.text);
        original.text = QStringLiteral("connection still usable");
        QVERIFY(store.capture(original, 1));
    }
    void hopyStreamsAndSkipsOversizedPayloads() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("images"))));
        QImage ordinary(32, 32, QImage::Format_ARGB32);
        ordinary.fill(Qt::blue);
        QVERIFY(ordinary.save(dir.filePath(QStringLiteral("images/ok.png"))));
        QImage tooWide(ClipboardRecord::maxImageSide + 1, 1, QImage::Format_ARGB32);
        tooWide.fill(Qt::red);
        QVERIFY(tooWide.save(dir.filePath(QStringLiteral("images/wide.png"))));
        const QString name = QStringLiteral("stream-fixture");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dir.filePath(QStringLiteral("clipboard.sqlite")));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE records(content TEXT,type INTEGER,"
                "created_at INTEGER,pinned INTEGER,favorite INTEGER,html_path TEXT,image_path TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO records VALUES"
                "('',2,1,0,0,'','ok.png'),('',2,2,0,0,'','ok.png'),"
                "('',2,3,0,0,'','wide.png')")));
            q.prepare(QStringLiteral("INSERT INTO records VALUES(?,3,4,0,0,'','')"));
            QStringList files;
            for (int i = 0; i < 1001; ++i)
                files << QStringLiteral("C:/fixture/%1").arg(i);
            q.addBindValue(files.join(u'\n'));
            QVERIFY(q.exec());
        }
        QSqlDatabase::removeDatabase(name);
        int delivered = 0;
        auto history = readHopyHistory(dir.path(), [&](ClipboardRecord r) -> std::expected<void, QString> {
            ++delivered;
            if (r.pendingImage.size() != QSize(32, 32))
                return std::unexpected(QStringLiteral("unexpected image size"));
            // The next row must not be decoded yet: remove its sidecar while
            // consuming the first row. A full-list reader would already have it.
            if (delivered == 1 && !QFile::remove(dir.filePath(QStringLiteral("images/ok.png"))))
                return std::unexpected(QStringLiteral("remove fixture failed"));
            r.finalize();
            return {};
        });
        QVERIFY(history);
        QCOMPARE(delivered, 1);
        QCOMPARE(history->imported, 1);
        QCOMPARE(history->skipped, 3);
    }
    void searchRestoresBrowseSelectionAfterAsyncResults() {
        Theme theme;
        theme.apply(QStringLiteral("dark"));
        ClipboardPanel panel(theme, false);
        ClipboardRecords rows;
        for (int i = 0; i < 50; ++i) {
            ClipboardRecord r;
            r.id = QString::number(i);
            r.text = QStringLiteral("record %1").arg(i);
            rows.append(r);
        }
        panel.setRecords(rows);
        panel.showNear({30, 30}, false);
        auto* list = panel.findChild<QListView*>();
        list->setCurrentIndex(list->model()->index(30, 0));
        list->scrollTo(list->currentIndex(), QAbstractItemView::PositionAtTop);
        const int scroll = list->verticalScrollBar()->value();
        QVERIFY(scroll > 0);
        panel.showSearch();
        panel.findChild<QLineEdit*>()->setText(QStringLiteral("record 2"));
        panel.setRecords({rows[2]});
        panel.closeSearch();
        QCOMPARE(list->verticalScrollBar()->maximum(), 0);
        // The service result arrives after closeSearch returned.
        panel.setRecords(rows);
        QCOMPARE(list->currentIndex().row(), 30);
        QCOMPARE(list->verticalScrollBar()->value(), scroll);
    }
    void pendingPreviewFollowsSelectionAndRejectsOldPayload() {
        Theme theme;
        ClipboardPanel panel(theme, false);
        ClipboardRecord a, b;
        a.id = a.text = QStringLiteral("A");
        b.id = b.text = QStringLiteral("B");
        panel.setRecords({a, b});
        panel.showNear({30, 30}, false);
        QSignalSpy requests(&panel, &ClipboardPanel::previewRequested);
        panel.handleKey(Qt::Key_Space);
        panel.handleKey(Qt::Key_Down);
        QCOMPARE(requests.size(), 2);
        auto* preview = panel.findChild<Panel*>();
        panel.displayPreview(a, requests[0][1].toULongLong());
        QVERIFY(!preview->isVisible());
        panel.displayPreview(b, requests[1][1].toULongLong());
        QVERIFY(preview->isVisible());
        panel.handleKey(Qt::Key_Up);
        const auto backToA = requests.last()[1].toULongLong();
        panel.handleKey(Qt::Key_Down);
        panel.displayPreview(a, backToA);
        QCOMPARE(preview->findChild<QPlainTextEdit*>()->toPlainText(), b.text);
        QSignalSpy paste(&panel, &ClipboardPanel::pasteRequested);
        panel.handleKey(Qt::Key_Return);
        QCOMPARE(paste[0][0].value<ClipboardRecord>().id, b.id);
    }
    void controlPOnlyPinsWhenPIsTheOpenKey() {
        Theme theme;
        ClipboardPanel panel(theme, false);
        ClipboardRecord r;
        r.id = QStringLiteral("link");
        r.text = QStringLiteral("https://example.com");
        panel.setRecords({r});
        ClipboardBehavior behavior;
        behavior.openKey = Qt::Key_P;
        panel.setBehavior(behavior);
        QSignalSpy flags(&panel, &ClipboardPanel::toggleFlagRequested);
        QSignalSpy opens(&panel, &ClipboardPanel::openRequested);
        panel.handleKey(Qt::Key_P, false, true);
        QCOMPARE(flags.size(), 1);
        QCOMPARE(opens.size(), 0);
        panel.handleKey(Qt::Key_P);
        QCOMPARE(opens.size(), 1);
    }
    void chineseTextIsSafeOnWorkers() {
        std::atomic<bool> valid{true};
        std::thread reader([&] {
            for (int i = 0; i < 20000; ++i) {
                const auto text = tx(u"剪贴板");
                if (text != QStringLiteral("剪贴板"))
                    valid = false;
            }
        });
        reader.join();
        QVERIFY(valid.load());
    }
    void queryBurstsCoalesceAndQueuedStaleQueriesYieldToWrites() {
        QTemporaryDir dir;
        ClipboardService service(dir.filePath(QStringLiteral("clips.sqlite")));
        QSignalSpy ready(&service, &ClipboardService::ready);
        QSignalSpy errors(&service, &ClipboardService::failed);
        QSignalSpy results(&service, &ClipboardService::recordsChanged);
        service.start(false);
        QTRY_COMPARE(ready.size(), 1);
        QTRY_VERIFY(!results.isEmpty());
        results.clear();
        QSignalSpy dispatches(&service.queryTimer_, &QTimer::timeout);
        QSemaphore entered, release;
        auto* gate = new QObject;
        gate->moveToThread(&service.thread_);
        QMetaObject::invokeMethod(gate, [&] { entered.release(); release.acquire(); },
                                  Qt::QueuedConnection);
        auto unblock = qScopeGuard([&] {
            release.release();
            QMetaObject::invokeMethod(gate, [] {}, Qt::BlockingQueuedConnection);
            gate->deleteLater();
        });
        QVERIFY(entered.tryAcquire(1, 2000));
        service.refresh(QStringLiteral("obsolete"));
        QTRY_COMPARE(dispatches.size(), 1);
        for (int i = 0; i < 100; ++i)
            service.refresh(QStringLiteral("old %1").arg(i));
        service.refresh();
        QTRY_COMPARE(dispatches.size(), 2); // one dispatch for the whole burst
        for (int i = 0; i < 4; ++i) {
            ClipboardRecord r;
            r.text = QStringLiteral("captured %1").arg(i);
            service.capture(r);
        }
        release.release();
        unblock.dismiss();
        gate->deleteLater();
        QTRY_COMPARE(service.pending_, 0);
        QTRY_VERIFY(!results.isEmpty() &&
                     results.last()[0].value<ClipboardRecords>().size() == 4);
        QCOMPARE(errors.size(), 0);
    }
    void unrelatedSettingsSurviveAnExistingHotkeyConflict() {
        if (!qEnvironmentVariableIsSet("MT_CLIPBOARD_NATIVE_TESTS"))
            QSKIP("Opt in to temporary native hotkey registration with MT_CLIPBOARD_NATIVE_TESTS=1");
        QTemporaryDir dir;
        auto config = Settings::defaults();
        config.clipboardHotkey = QStringLiteral("Ctrl+Alt+Shift+F23");
        config.launcherHotkey = QStringLiteral("Ctrl+Alt+Shift+F24");
        SettingsStore store(dir.path());
        QVERIFY(store.save(config));
        DesktopServices blocker(dir.path() + QStringLiteral("-blocker"));
        QVERIFY(blocker.setHotkeys({{9, config.launcherHotkey}}));
        DesktopServices desktop(dir.path());
        Application app(dir.path(), true, desktop);
        QVERIFY(app.start());
        app.clipboardPanel_->hide();
        app.preview_ = false;
        QVERIFY(!desktop.setHotkeys(app.shortcuts(app.settings_), true));
        auto next = app.settings_;
        next.clipboardPaused = true;
        next.historyLimit = 42;
        bool completed = false;
        std::expected<void, QString> result;
        app.applySettings(next, [&](auto r) { result = r; completed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
        QVERIFY2(result, result ? "" : qPrintable(result.error()));
        const auto saved = store.load();
        QVERIFY(saved);
        QVERIFY(saved->clipboardPaused);
        QCOMPARE(saved->historyLimit, 42);
        // A newly requested conflict still fails instead of silently degrading.
        next.launcherEnabled = false;
        next.clipboardHotkey = config.launcherHotkey;
        completed = false;
        app.applySettings(next, [&](auto r) { result = r; completed = true; });
        QTRY_VERIFY(completed);
        QVERIFY(!result);
        QCOMPARE(store.load()->clipboardHotkey, config.clipboardHotkey);
    }
    void reopeningCancelsPendingPasteAndRetrieval() {
        QTemporaryDir dir;
        DesktopServices desktop(dir.path());
        Application app(dir.path(), true, desktop);
        QVERIFY(app.start());
        app.clipboardPanel_->hide();
        auto* timer = new QTimer(&app);
        app.pasteOperation_ = timer;
        timer->setSingleShot(true);
        bool fired = false;
        connect(timer, &QTimer::timeout, &app, [&] { fired = true; });
        const auto oldGeneration = app.pasteGeneration_;
        timer->start(10);
        app.showClipboard();
        QVERIFY(app.pasteGeneration_ != oldGeneration);
        QVERIFY(!timer->isActive());
        QVERIFY(app.pasteOperation_.isNull());
        QTest::qWait(30);
        QVERIFY(!fired);
    }
    void searchKeepsMouseHookAndDismissalReleasesIt() {
        if (!qEnvironmentVariableIsSet("MT_CLIPBOARD_NATIVE_TESTS"))
            QSKIP("Opt in to temporary native panel hooks with MT_CLIPBOARD_NATIVE_TESTS=1");
        QTemporaryDir dir;
        DesktopServices desktop(dir.path());
        Application app(dir.path(), true, desktop);
        QVERIFY(app.start());
        app.preview_ = false;
        QVERIFY(desktop.capturePanelNavigation(true));
        QCOMPARE(desktop.panelInputCaptured(), std::make_pair(true, true));
        app.clipboardPanel_->showSearch();
        QCOMPARE(desktop.panelInputCaptured(), std::make_pair(false, true));
        QTest::qWait(220); // the appearance guard intentionally ignores the first 200 ms
        Q_EMIT desktop.outsidePress();
        QVERIFY(!app.clipboardPanel_->isVisible());
        QCOMPARE(desktop.panelInputCaptured(), std::make_pair(false, false));
    }
    void caretRequestsUseMtaAndDiscardExpiredContext() {
        CaretLocator locator;
        int callbacks = 0;
        auto* context = new QObject;
        locator.request(0, context, [&](auto, auto) { ++callbacks; });
        delete context;
        QString trace;
        locator.request(0, this, [&](auto, QString detail) {
            QVERIFY(QThread::currentThread() == qApp->thread());
            trace = detail;
            ++callbacks;
        });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 5000);
        QVERIFY(trace.contains(QStringLiteral("caretThread=MTA")));
    }
    void ownEditorCaretQueryDoesNotBlockGui() {
        QLineEdit editor;
        editor.setText(QStringLiteral("synthetic caret fixture"));
        editor.resize(360, 40);
        editor.show();
        editor.activateWindow();
        editor.setFocus();
        const auto handle = reinterpret_cast<HWND>(editor.winId());
        if (!SetForegroundWindow(handle) && GetForegroundWindow() != handle)
            QSKIP("Windows foreground policy denied activation of the synthetic editor");
        CaretLocator locator;
        bool finished = false;
        int guiTicks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++guiTicks; });
        heartbeat.start();
        std::optional<QRect> found;
        QString diagnostic;
        locator.request(reinterpret_cast<quintptr>(handle), this,
            [&](auto caret, QString trace) { found = caret; diagnostic = trace; finished = true; });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY(guiTicks > 0);
        QVERIFY2(found, qPrintable(diagnostic));
        QVERIFY(diagnostic.contains(QStringLiteral("caretThread=MTA")));
    }
};
} // namespace mt
QTEST_MAIN(mt::ClipboardIntegrationTests)
#include "ClipboardIntegrationTests.moc"
