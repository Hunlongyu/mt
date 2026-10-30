#include "infrastructure/settings/Settings.h"
#include "infrastructure/i18n/I18n.h"
#include "infrastructure/storage/ClipboardStore.h"
#include "infrastructure/storage/HopyImport.h"
#include "infrastructure/storage/HLaunchImport.h"
#include "modules/launcher/EdgeTrigger.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include "modules/clipboard/ClipboardText.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QRegularExpression>
#include "modules/gestures/GestureRecognizer.h"
#include "modules/launcher/LauncherCatalog.h"
#include "modules/launcher/LauncherService.h"
#include "modules/screen_lock/UnlockPolicy.h"
#include "modules/screen_lock/LockInput.h"
#include <QFile>
#include <QJsonDocument>
#include <QMimeData>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QtPlugin>
#include <QtTest>

Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)

using namespace mt;
class BehaviorTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void organizationRenamePreservesExistingDataRoot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto preferred = dir.filePath(QStringLiteral("dev.hly.com/mt"));
        const auto legacy = dir.filePath(QStringLiteral("HunlongyuDevelopment/mt"));
        QCOMPARE(compatibleDefaultDataRoot(preferred, legacy), preferred);
        QVERIFY(!QFileInfo::exists(preferred));
        QVERIFY(QDir().mkpath(legacy));
        auto original = Settings::defaults();
        original.launcherColumns = 9;
        SettingsStore oldStore(legacy);
        QVERIFY(oldStore.save(original));
        QCOMPARE(compatibleDefaultDataRoot(preferred, legacy), legacy);
        SettingsStore existing(compatibleDefaultDataRoot(preferred, legacy));
        QVERIFY(existing.load());
        QCOMPARE(existing.load()->launcherColumns, 9);
        QVERIFY(!QFileInfo::exists(preferred));
        QVERIFY(QDir().mkpath(preferred));
        QCOMPARE(compatibleDefaultDataRoot(preferred, legacy), preferred);
        // Even a damaged preferred profile must not silently select stale data.
        QFile damaged(QDir(preferred).filePath(QStringLiteral("settings.json")));
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write("invalid json");
        damaged.close();
        SettingsStore selected(compatibleDefaultDataRoot(preferred, legacy));
        QVERIFY(!selected.load());
        QVERIFY(!selected.save(Settings::defaults()));
        QCOMPARE(oldStore.load()->launcherColumns, 9);
    }
    void opacitySettingsPreserveLegacyDefaultsAndRejectInvalidValues() {
        const auto legacy = Settings::decode(R"({"schema":1})");
        QVERIFY(legacy);
        QCOMPARE(legacy->launcherOpacity, 95);
        QCOMPARE(legacy->clipboardOpacity, 95);
        auto settings = *legacy;
        settings.launcherOpacity = 30;
        settings.clipboardOpacity = 60;
        const auto restored = Settings::decode(settings.encode());
        QVERIFY(restored);
        QCOMPARE(restored->launcherOpacity, 30);
        QCOMPARE(restored->clipboardOpacity, 60);
        for (const auto value : {QByteArrayLiteral("29"), QByteArrayLiteral("101"),
                                 QByteArrayLiteral("75.5"), QByteArrayLiteral("\"95\"")})
            QVERIFY(!Settings::decode(QByteArrayLiteral("{\"schema\":1,\"launcherOpacity\":") +
                                      value + '}'));
        QVERIFY(!Settings::decode(R"({"schema":1,"clipboardOpacity":59})"));
    }
    void summariesNeverReplaceFullPayload() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord r;
        r.text = QString(20000, u'文');
        r.html = QStringLiteral("<p>") + r.text + QStringLiteral("</p>");
        r.kind = ClipKind::Html;
        r.finalize();
        QVERIFY(store.capture(r, 10));
        const auto rows = store.records();
        QVERIFY(rows);
        QCOMPARE(rows->size(), 1);
        QVERIFY(rows->front().summaryOnly);
        QVERIFY(rows->front().html.isEmpty());
        QCOMPARE(rows->front().text.size(), 4096);
        const auto full = store.record(rows->front().id);
        QVERIFY(full);
        QVERIFY(!full->summaryOnly);
        QCOMPARE(full->text, r.text);
        QCOMPARE(full->html, r.html);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, rows->front().mime());
    }
    void malformedKnownSettingsAreNotSilentlyDefaulted() {
        auto o = QJsonDocument::fromJson(Settings::defaults().encode()).object();
        o[QStringLiteral("historyLimit")] = QStringLiteral("500");
        QVERIFY(!Settings::decode(QJsonDocument(o).toJson()));
        o[QStringLiteral("historyLimit")] = 7.5;
        QVERIFY(!Settings::decode(QJsonDocument(o).toJson()));
    }
    void sensitiveMimePreservesMarker() {
        ClipboardRecord r;
        r.text = QStringLiteral("synthetic");
        r.sensitive = true;
        QVERIFY(r.mime()->hasFormat(QStringLiteral(
            "application/"
            "x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\"")));
    }
    void settingsRoundTripAndUnknownFields() {
        auto input = Settings::defaults();
        input.theme = QStringLiteral("light");
        input.historyLimit = 123;
        input.extra.insert(QStringLiteral("futureValue"), 17);
        auto decoded = Settings::decode(input.encode());
        QVERIFY(decoded);
        QCOMPARE(decoded->theme, input.theme);
        QCOMPARE(decoded->historyLimit, 123);
        QCOMPARE(decoded->extra[QStringLiteral("futureValue")].toInt(), 17);
    }
    void corruptSettingsAreNeverOverwritten() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("settings.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray broken("{incomplete");
        file.write(broken);
        file.close();
        SettingsStore store(dir.path());
        QVERIFY(!store.load());
        QVERIFY(!store.save(Settings::defaults()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), broken);
    }
    void newerSettingsAreRejected() {
        auto object = QJsonDocument::fromJson(Settings::defaults().encode()).object();
        object[QStringLiteral("schema")] = 2;
        QVERIFY(!Settings::decode(QJsonDocument(object).toJson()));
    }
    void shortcutCollisionIsRejected() {
        auto s = Settings::defaults();
        s.launcherHotkey = QStringLiteral("alt+c");
        QVERIFY(!s.validate());
        s.launcherEnabled = false;
        QVERIFY(s.validate());
    }
    void payloadPriorityAndRoundTrip() {
        QMimeData mime;
        mime.setText(QStringLiteral("fallback"));
        mime.setHtml(QStringLiteral("<b>富文本</b>"));
        mime.setImageData(QImage(4, 4, QImage::Format_ARGB32));
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/测试/a.txt"))});
        auto record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Files);
        record->finalize();
        auto output = record->mime();
        QCOMPARE(output->urls().front().toLocalFile(), QStringLiteral("C:/测试/a.txt"));
        mime.setUrls({});
        record = ClipboardRecord::snapshot(mime);
        QVERIFY(record);
        QCOMPARE(record->kind, ClipKind::Html);
        record->finalize();
        QCOMPARE(record->mime()->html(), mime.html());
        QVERIFY(!record->mime(true)->hasHtml());
    }
    void rejectOversizeText() {
        QMimeData mime;
        mime.setText(QString(1'000'001, u'x'));
        QVERIFY(!ClipboardRecord::snapshot(mime));
    }
    void encodedImageIsStable() {
        QMimeData mime;
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        mime.setImageData(image);
        auto a = ClipboardRecord::snapshot(mime);
        auto b = ClipboardRecord::snapshot(mime);
        QVERIFY(a && b);
        a->finalize();
        b->finalize();
        QCOMPARE(a->id, b->id);
        QVERIFY(!a->image.isEmpty());
        QCOMPARE(QImage::fromData(a->image), image);
    }
    void historyLimitExcludesFavoritesAndPinned() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        for (int i = 0; i < 8; ++i) {
            ClipboardRecord r;
            r.text = QString::number(i);
            r.captured = QDateTime::fromMSecsSinceEpoch(i * 1000);
            r.favorite = i == 0;
            r.pinned = i == 1;
            QVERIFY(store.capture(r, 3));
        }
        auto rows = store.records();
        QVERIFY(rows);
        QCOMPARE(rows->size(), 5);
        QCOMPARE(rows->front().text, QStringLiteral("1"));
        int ordinary = 0;
        for (const auto& r : *rows)
            if (!r.favorite && !r.pinned)
                ++ordinary;
        QCOMPARE(ordinary, 3);
    }
    void duplicateCapturePreservesFlagsAndSensitivity() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord r;
        r.text = QStringLiteral("secret-test-fixture");
        r.sensitive = true;
        r.finalize();
        QVERIFY(store.capture(r, 10));
        QVERIFY(store.setFlags(r.id, true, true));
        r.sensitive = false;
        r.favorite = false;
        r.pinned = false;
        QVERIFY(store.capture(r, 10));
        auto rows = store.records();
        QVERIFY(rows);
        QCOMPARE(rows->size(), 1);
        QVERIFY(rows->front().favorite);
        QVERIFY(rows->front().pinned);
        QVERIFY(rows->front().sensitive);
    }
    void filtersAndLiteralSearch() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord text;
        text.text = QStringLiteral("100%_完成");
        QVERIFY(store.capture(text, 100));
        ClipboardRecord other;
        other.text = QStringLiteral("100 percent complete");
        QVERIFY(store.capture(other, 100));
        ClipboardRecord file;
        file.kind = ClipKind::Files;
        file.files = {QStringLiteral("C:/a.txt")};
        file.text = file.files.front();
        file.favorite = true;
        QVERIFY(store.capture(file, 100));
        auto rows = store.records(QStringLiteral("%_"));
        QVERIFY(rows);
        QCOMPARE(rows->size(), 1);
        QCOMPARE(rows->front().text, text.text);
        rows = store.records({}, 2);
        QVERIFY(rows);
        QCOMPARE(rows->size(), 1);
        QCOMPARE(rows->front().kind, ClipKind::Files);
        rows = store.records({}, 3);
        QCOMPARE(rows->size(), 1);
    }
    void failedCaptureDoesNotPoisonRetry() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord r;
        r.text = QStringLiteral("retry");
        QVERIFY(!store.capture(r, 0));
        QVERIFY(store.capture(r, 1));
        auto rows = store.records();
        QVERIFY(rows);
        QCOMPARE(rows->size(), 1);
    }
    void futureDatabaseIsPreserved() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("future.sqlite"));
        const QString connection = QStringLiteral("future-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=99")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        ClipboardStore store(path);
        QVERIFY(!store.open());
    }
    void clipboardSettingsRoundTripAndMigrate() {
        // Before storageLimit existed, historyLimit also bounded storage.
        auto legacy = Settings::decode(R"({"schema":1,"historyLimit":3000})");
        QVERIFY(legacy);
        QCOMPARE(legacy->storageLimit, 3000);
        QCOMPARE(Settings::decode(R"({"schema":1})")->storageLimit, 1000);
        QVERIFY(legacy->pasteOnConfirm && legacy->hoverPreview && legacy->spacePreview);
        QCOMPARE(legacy->openKey, QStringLiteral("O"));
        QCOMPARE(legacy->openMouseButton, QStringLiteral("middle"));
        auto s = *legacy;
        s.startHidden = false;
        s.theme = QStringLiteral("auto");
        s.pasteOnConfirm = false;
        s.clipboardSuppressFullscreen = false;
        s.hoverPreview = false;
        s.spacePreview = false;
        s.previewSide = QStringLiteral("right");
        s.clipboardPlacement = QStringLiteral("center");
        s.openKey = QString{};
        s.openMouseButton = QStringLiteral("right");
        s.storageLimit = 5000;
        const auto back = Settings::decode(s.encode());
        QVERIFY(back);
        QCOMPARE(back->startHidden, s.startHidden);
        QCOMPARE(back->theme, s.theme);
        QVERIFY(!back->pasteOnConfirm && !back->clipboardSuppressFullscreen);
        QVERIFY(!back->hoverPreview && !back->spacePreview);
        QCOMPARE(back->previewSide, s.previewSide);
        QCOMPARE(back->clipboardPlacement, s.clipboardPlacement);
        QVERIFY(back->openKey.isEmpty());
        QCOMPARE(back->openMouseButton, s.openMouseButton);
        QCOMPARE(back->storageLimit, 5000);
        for (const auto* bad :
             {R"({"schema":1,"openKey":"F"})", R"({"schema":1,"openKey":"OK"})",
              R"({"schema":1,"previewSide":"top"})", R"({"schema":1,"openMouseButton":"left"})",
              R"({"schema":1,"startHidden":"yes"})", R"({"schema":1,"historyLimit":900,"storageLimit":800})"})
            QVERIFY2(!Settings::decode(bad), bad);
    }
    void contentAwareHelpers() {
        QCOMPARE(clipColor(QStringLiteral("#4DAAFC")), QColor(0x4D, 0xAA, 0xFC));
        QCOMPARE(clipColor(QStringLiteral("#fff")), QColor(255, 255, 255));
        QCOMPARE(clipColor(QStringLiteral("color: rgb(255, 85, 0);")), QColor(255, 85, 0));
        QCOMPARE(clipColor(QStringLiteral("ff5500")), QColor(0xFF, 0x55, 0x00));
        QCOMPARE(clipColor(QStringLiteral("#11223380")).alpha(), 0x80);
        QVERIFY(!clipColor(QStringLiteral("rgb(300,0,0)")).isValid());
        QVERIFY(!clipColor(QStringLiteral("commit 4daafc1e9b")).isValid());
        QVERIFY(!clipColor(QString(40, u'a') + QStringLiteral(" #fff")).isValid());
        QCOMPARE(openTarget(QStringLiteral(" https://example.com ")).kind, OpenKind::Url);
        QCOMPARE(openTarget(QStringLiteral("www.example.com")).value,
                 QStringLiteral("https://www.example.com"));
        QCOMPARE(openTarget(QStringLiteral("hello@example.com")).kind, OpenKind::Email);
        QCOMPARE(openTarget(QStringLiteral("C:\\Users\\Public")).kind, OpenKind::Path);
        QCOMPARE(openTarget(QStringLiteral("plain words")).kind, OpenKind::None);
        QCOMPARE(openTarget(QStringLiteral("https://a\nhttps://b")).kind, OpenKind::None);
        QCOMPARE(clipCharacters(QStringLiteral("a😀b")), 3);
        QCOMPARE(clipLines(QString{}), 0);
        QCOMPARE(clipLines(QStringLiteral("a\nb")), 2);
        const auto now = QDateTime::currentDateTimeUtc();
        QCOMPARE(clipRelativeTime(now.addSecs(-10), now), QStringLiteral("刚刚"));
        QCOMPARE(clipRelativeTime(now.addSecs(-300), now), QStringLiteral("5 分钟前"));
        const auto older = now.addDays(-400);
        QCOMPARE(clipRelativeTime(older, now), older.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")));
    }
    void displayAndStorageLimitsAreSeparate() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        for (int i = 0; i < 10; ++i) {
            ClipboardRecord r;
            r.text = QStringLiteral("item %1").arg(i);
            r.captured = QDateTime::fromMSecsSinceEpoch((i + 1) * 1000);
            r.favorite = i == 0;
            QVERIFY(store.capture(r, 6));
        }
        // 1 favorite + 6 stored ordinary; the panel shows the newest 3 ordinary.
        auto shown = store.records({}, -1, 3);
        QVERIFY(shown);
        QCOMPARE(shown->size(), 4);
        QVERIFY(std::ranges::any_of(*shown, [](const auto& r) { return r.favorite; }));
        auto all = store.records({}, -1, 100);
        QCOMPARE(all->size(), 7);
        // Search applies the display limit to ordinary results only.
        auto found = store.records(QStringLiteral("item"), -1, 2);
        QCOMPARE(found->size(), 3);
    }
    void confirmedRecordMovesToTopAndClearEmpties() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord old;
        old.text = QStringLiteral("old");
        old.captured = QDateTime::fromMSecsSinceEpoch(1000);
        ClipboardRecord recent;
        recent.text = QStringLiteral("recent");
        // Moving a record must still work if the wall clock has moved backwards.
        // This also reproduces a tied clock tick without relying on sleeps.
        recent.captured = QDateTime::currentDateTimeUtc().addSecs(60);
        QVERIFY(store.capture(old, 10));
        QVERIFY(store.capture(recent, 10));
        auto rows = store.records();
        QCOMPARE(rows->front().text, QStringLiteral("recent"));
        QVERIFY(store.touch(rows->back().id));
        rows = store.records();
        QCOMPARE(rows->front().text, QStringLiteral("old"));
        QVERIFY(store.clear());
        QVERIFY(store.records()->isEmpty());
    }
    void importMergesWithoutClearingFlags() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("clips.sqlite")));
        QVERIFY(store.open());
        ClipboardRecord mine;
        mine.text = QStringLiteral("shared");
        QVERIFY(store.capture(mine, 10));
        QVERIFY(store.setFlags(store.records()->front().id, true, false));
        ClipboardRecord imported;
        imported.text = QStringLiteral("shared");
        imported.pinned = true;
        imported.captured = QDateTime::fromMSecsSinceEpoch(1000);
        ClipboardRecord extra;
        extra.text = QStringLiteral("only in hopy");
        extra.captured = QDateTime::fromMSecsSinceEpoch(2000);
        const auto count = store.import({imported, extra}, 10);
        QVERIFY(count);
        QCOMPARE(*count, 2);
        const auto rows = store.records();
        QCOMPARE(rows->size(), 2);
        const auto shared = std::ranges::find_if(*rows, [](const auto& r) {
            return r.text == QStringLiteral("shared");
        });
        QVERIFY(shared->favorite && shared->pinned);
        // The newer local capture time wins over the older imported one.
        QVERIFY(shared->captured.toMSecsSinceEpoch() > 1000);
    }
    static QByteArray fileHash(const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly)
                   ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256)
                   : QByteArray{};
    }
    void hopyHistoryIsReadWithoutTouchingTheSource() {
        QTemporaryDir dir;
        const auto folder = dir.filePath(QStringLiteral("hopy"));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/images")));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/rich_text")));
        QImage image(8, 6, QImage::Format_ARGB32);
        image.fill(Qt::red);
        // Stored with a stale absolute path: the copy under images/ must be used.
        QVERIFY(image.save(folder + QStringLiteral("/images/42.png")));
        QFile html(folder + QStringLiteral("/rich_text/7.html"));
        QVERIFY(html.open(QIODevice::WriteOnly));
        html.write("<b>bold</b>");
        html.close();
        const auto db = folder + QStringLiteral("/clipboard.sqlite");
        {
            auto c = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("hopy-fixture"));
            c.setDatabaseName(db);
            QVERIFY(c.open());
            QSqlQuery q(c);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO meta VALUES('schema_version','1')")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE records (id INTEGER PRIMARY KEY, content TEXT NOT NULL, type INTEGER, "
                "created_at INTEGER, pinned INTEGER DEFAULT 0, favorite INTEGER DEFAULT 0, "
                "html_path TEXT, rtf_path TEXT, image_path TEXT, sensitive INTEGER DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO records VALUES(1,'hello',0,1000,1,0,NULL,NULL,NULL,1),"
                "(2,'bold',1,2000,0,1,'Z:/gone/rich_text/7.html',NULL,NULL,0),"
                "(3,'Z:/gone/images/42.png',2,3000,0,0,NULL,NULL,'Z:/gone/images/42.png',0),"
                "(4,'C:/a.txt\nC:/b.txt',3,4000,0,0,NULL,NULL,NULL,0),"
                "(5,'missing',2,5000,0,0,NULL,NULL,'Z:/gone/images/none.png',0)")));
            c.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("hopy-fixture"));
        const auto before = fileHash(db);
        ClipboardRecords importedRows;
        const auto history = readHopyHistory(folder, [&importedRows](ClipboardRecord r)
                -> std::expected<void, QString> {
            importedRows.append(std::move(r));
            return {};
        });
        QVERIFY2(history, history ? "" : qPrintable(history.error()));
        QCOMPARE(fileHash(db), before);
        QCOMPARE(history->skipped, 1);
        QCOMPARE(history->imported, 4);
        const auto& text = importedRows[0];
        QCOMPARE(text.text, QStringLiteral("hello"));
        QVERIFY(text.pinned && text.sensitive);
        QCOMPARE(text.captured.toMSecsSinceEpoch(), 1000);
        QCOMPARE(importedRows[1].kind, ClipKind::Html);
        QCOMPARE(importedRows[1].html, QStringLiteral("<b>bold</b>"));
        QVERIFY(importedRows[1].favorite);
        QCOMPARE(importedRows[2].kind, ClipKind::Image);
        QCOMPARE(importedRows[2].pendingImage.size(), QSize(8, 6));
        QCOMPARE(importedRows[3].files, QStringList({QStringLiteral("C:/a.txt"), QStringLiteral("C:/b.txt")}));
        QVERIFY(!readHopyHistory(dir.filePath(QStringLiteral("empty")),
            [](ClipboardRecord) -> std::expected<void, QString> { return {}; }));
        ClipboardStore store(dir.filePath(QStringLiteral("mt.sqlite")));
        QVERIFY(store.open());
        QCOMPARE(*store.import(importedRows, 100), 4);
    }
    void interfaceIsAlwaysChinese() {
        QCOMPARE(tx(u"剪贴板"), QStringLiteral("剪贴板"));
        const auto decoded = Settings::decode(R"({"schema":1,"language":"en"})");
        QVERIFY(decoded);
        QVERIFY(!QJsonDocument::fromJson(decoded->encode()).object().contains(QStringLiteral("language")));
    }
    static LaunchItem item(const QString& name) {
        LaunchItem i;
        i.name = name;
        i.target = QStringLiteral("C:/fixtures/") + name + QStringLiteral(".exe");
        return i;
    }
    void emptyDestinationKeepsSourceHole() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("a"))));
        const auto id = c.categories[0].slots[0];
        QVERIFY(c.move(0, 0, 0, 5));
        QVERIFY(c.categories[0].slots[0].isEmpty());
        QCOMPARE(c.categories[0].slots[5], id);
        QVERIFY(c.validate());
    }
    void occupiedSameCategoryShiftsInterval() {
        auto c = LauncherCatalog::empty();
        for (int i = 0; i < 3; ++i)
            QVERIFY(c.put(0, i, item(QString::number(i))));
        const auto original = c.categories[0].slots;
        QVERIFY(c.move(0, 0, 0, 2));
        QCOMPARE(c.categories[0].slots[0], original[1]);
        QCOMPARE(c.categories[0].slots[1], original[2]);
        QCOMPARE(c.categories[0].slots[2], original[0]);
        QVERIFY(c.validate());
    }
    void crossCategoryShiftsUntilVacancy() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 2, item(QStringLiteral("a"))));
        QVERIFY(c.put(1, 4, item(QStringLiteral("b"))));
        QVERIFY(c.put(1, 5, item(QStringLiteral("c"))));
        const auto moved = c.categories[0].slots[2];
        const auto shifted = c.categories[1].slots[5];
        QVERIFY(c.move(0, 2, 1, 4));
        QVERIFY(c.categories[0].slots[2].isEmpty());
        QCOMPARE(c.categories[1].slots[4], moved);
        QCOMPARE(c.categories[1].slots[6], shifted);
        QVERIFY(c.validate());
    }
    void deletionDoesNotCompactGrid() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("a"))));
        QVERIFY(c.put(0, 3, item(QStringLiteral("b"))));
        const auto b = c.categories[0].slots[3];
        QVERIFY(c.erase(0, 0));
        QCOMPARE(c.categories[0].slots[3], b);
        QCOMPARE(c.categories[0].slots.size(), 28);
        QVERIFY(c.validate());
    }
    void catalogRoundTripRetainsEmptySlots() {
        QTemporaryDir dir;
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(2, 29, item(QStringLiteral("中文"))));
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(c.save(path));
        auto loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->categories[2].slots, c.categories[2].slots);
        QCOMPARE(loaded->selectedCategory, c.selectedCategory);
    }
    void failedSaveDoesNotPublishInMemoryMutation() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        LauncherService service(path);
        QVERIFY(service.load());
        const auto selected = service.catalog().selectedCategory;
        QDir().mkdir(path);
        QSignalSpy changes(&service, &LauncherService::changed);
        QSignalSpy failures(&service, &LauncherService::failed);
        QVERIFY(service.put(selected, 0, item(QStringLiteral("unsaved"))));
        QTRY_VERIFY(!service.busy());
        QCOMPARE(failures.size(), 1);
        QVERIFY(service.catalog().items.isEmpty());
        QCOMPARE(changes.count(), 0);
    }
    void crossCategorySearchRanksExactFirst() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("Notepad++"))));
        QVERIFY(c.put(1, 0, item(QStringLiteral("Note"))));
        QVERIFY(c.put(2, 0, item(QStringLiteral("notes"))));
        auto matches = c.search(QStringLiteral("NOTE"));
        QCOMPARE(matches.size(), 3);
        QCOMPARE(c.items[matches.front().itemId].name, QStringLiteral("Note"));
    }
    void searchPrefersUsageWithinTheSameMatchTier() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("Note A"))));
        QVERIFY(c.put(0, 1, item(QStringLiteral("Note B"))));
        QVERIFY(c.put(0, 2, item(QStringLiteral("Note"))));
        const auto b = c.categories[0].slots[1];
        QVERIFY(c.recordLaunch(b, QDateTime::currentDateTimeUtc()));
        const auto matches = c.search(QStringLiteral("note"));
        QCOMPARE(matches.size(), 3);
        // Exact still beats a frequently used prefix match.
        QCOMPARE(c.items[matches[0].itemId].name, QStringLiteral("Note"));
        QCOMPARE(matches[1].itemId, b);
        QVERIFY(!c.recordLaunch(QStringLiteral("missing"), QDateTime::currentDateTimeUtc()));
    }
    void launchStatisticsRoundTripAndOldFilesLoad() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("a"))));
        const auto id = c.categories[0].slots[0];
        const auto when = QDateTime(QDate(2026, 10, 29), QTime(8, 30), QTimeZone::UTC);
        QVERIFY(c.recordLaunch(id, when));
        QVERIFY(c.recordLaunch(id, when));
        QVERIFY(c.save(path));
        auto loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->items[id].launchCount, 2);
        QCOMPARE(loaded->items[id].lastLaunched, when);
        // Files written before statistics existed have neither field.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto text = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY(text.contains(QStringLiteral("lastLaunchedAt")));
        auto root = QJsonDocument::fromJson(text.toUtf8()).object();
        auto entries = root[QStringLiteral("items")].toArray();
        auto entry = entries[0].toObject();
        entry.remove(QStringLiteral("launchCount"));
        entry.remove(QStringLiteral("lastLaunchedAt"));
        entries[0] = entry;
        root[QStringLiteral("items")] = entries;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->items[id].launchCount, 0);
        QVERIFY(!loaded->items[id].lastLaunched.isValid());
    }
    void deletingCategoryKeepsItsItems() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 3, item(QStringLiteral("kept"))));
        QVERIFY(c.put(1, 0, item(QStringLiteral("x"))));
        QVERIFY(c.put(1, 5, item(QStringLiteral("y"))));
        const auto x = c.categories[1].slots[0];
        const auto y = c.categories[1].slots[5];
        c.selectedCategory = c.categories[1].id;
        QVERIFY(c.removeCategory(1));
        QCOMPARE(c.categories.size(), 3);
        // Appended after the neighbour's last occupied slot, in order.
        QCOMPARE(c.categories[0].slots[4], x);
        QCOMPARE(c.categories[0].slots[5], y);
        QCOMPARE(c.items.size(), 3);
        QCOMPARE(c.selectedCategory, c.categories[0].id);
        QVERIFY(c.validate());
        // The first category hands its items to the next one.
        QVERIFY(c.removeCategory(0));
        QCOMPARE(c.items.size(), 3);
        QVERIFY(c.validate());
        while (c.categories.size() > 1)
            QVERIFY(c.removeCategory(0));
        QVERIFY(!c.removeCategory(0));
        QCOMPARE(c.items.size(), 3);
    }
    void moveToCategoryAppendsAtEnd() {
        auto c = LauncherCatalog::empty();
        QVERIFY(c.put(0, 0, item(QStringLiteral("a"))));
        QVERIFY(c.put(1, 6, item(QStringLiteral("b"))));
        const auto a = c.categories[0].slots[0];
        QVERIFY(c.moveToCategory(a, 1));
        QVERIFY(c.categories[0].slots[0].isEmpty());
        QCOMPARE(c.categories[1].slots[7], a);
        QVERIFY(c.moveToCategory(a, 1)); // already there: unchanged
        QCOMPARE(c.categories[1].slots[7], a);
        QVERIFY(c.validate());
    }
    void duplicatesAreSkippedWhenAdding() {
        QTemporaryDir dir;
        LauncherService service(dir.filePath(QStringLiteral("launcher.json")));
        QVERIFY(service.load());
        const auto category = service.catalog().selectedCategory;
        auto a = item(QStringLiteral("a"));
        auto elevated = a;
        elevated.elevated = true;
        auto renamed = a;
        renamed.name = QStringLiteral("other name");
        renamed.target = renamed.target.toUpper(); // paths compare case-insensitively
        int skipped = -1;
        service.addItems(category, 0, {a, elevated, renamed}, [&](auto result) { if (result) skipped = *result; });
        QTRY_VERIFY(!service.busy());
        QCOMPARE(skipped, 1);
        QCOMPARE(service.catalog().items.size(), 2);
        const auto index = service.catalog().categoryIndex(category);
        QVERIFY(!service.catalog().categories[index].slots[0].isEmpty());
        QVERIFY(!service.catalog().categories[index].slots[1].isEmpty());
    }
    void commandLineQuotingFollowsWindowsRules() {
        QCOMPARE(LauncherCatalog::quoteArgument(QStringLiteral("plain")), QStringLiteral("plain"));
        QCOMPARE(LauncherCatalog::quoteArgument({}), QStringLiteral("\"\""));
        QCOMPARE(LauncherCatalog::quoteArgument(QStringLiteral("C:\\a b\\")),
                 QStringLiteral("\"C:\\a b\\\\\""));
        QCOMPARE(LauncherCatalog::quoteArgument(QStringLiteral("say \"hi\"")),
                 QStringLiteral("\"say \\\"hi\\\"\""));
        LaunchItem i;
        i.target = QStringLiteral("C:\\Program Files\\App\\app.exe");
        i.arguments = QStringLiteral("--x 1");
        QCOMPARE(LauncherCatalog::commandLine(i),
                 QStringLiteral("\"C:\\Program Files\\App\\app.exe\" --x 1"));
        QVERIFY(LauncherCatalog::acceptsFileArguments(i));
        i.target = QStringLiteral("https://example.com/a b");
        QVERIFY(LauncherCatalog::isUrl(i.target));
        QVERIFY(!LauncherCatalog::acceptsFileArguments(i));
        i.target = QStringLiteral("D:\\notes.txt");
        QVERIFY(!LauncherCatalog::isUrl(i.target));
        QVERIFY(!LauncherCatalog::acceptsFileArguments(i));
    }
    void hlaunchImportRemapsAndNeverTouchesSource() {
        QTemporaryDir source, target;
        QDir exe(source.path());
        QVERIFY(exe.mkpath(QStringLiteral("data/shortcuts")));
        const auto data = exe.filePath(QStringLiteral("data"));
        QFile link(QDir(data).filePath(QStringLiteral("shortcuts/kept.lnk")));
        QVERIFY(link.open(QIODevice::WriteOnly));
        link.write("shortcut bytes");
        link.close();
        const auto entry = [](const QString& name, const QString& type, const QString& target,
                              const QJsonValue& slot) {
            return QJsonObject{{QStringLiteral("id"), LauncherCatalog::newId()},
                               {QStringLiteral("type"), type},
                               {QStringLiteral("name"), name},
                               {QStringLiteral("target"), target},
                               {QStringLiteral("arguments"),
                                QJsonArray{QStringLiteral("--profile"), QStringLiteral("a b")}},
                               {QStringLiteral("workingDirectory"), QJsonValue()},
                               {QStringLiteral("icon"), QJsonValue()},
                               {QStringLiteral("runAsAdministrator"), false},
                               {QStringLiteral("launchCount"), 7},
                               {QStringLiteral("lastLaunchedAt"), QStringLiteral("2026-08-20T09:30:00Z")},
                               {QStringLiteral("gridSlot"), slot}};
        };
        const QJsonArray items{
            entry(QStringLiteral("rel"), QStringLiteral("application"), QStringLiteral("tools\\a.exe"), 6),
            entry(QStringLiteral("fifth"), QStringLiteral("application"), QStringLiteral("C:\\x\\e.exe"), 4),
            entry(QStringLiteral("web"), QStringLiteral("url"), QStringLiteral("https://example.com"), 0),
            entry(QStringLiteral("env"), QStringLiteral("file"), QStringLiteral("%USERPROFILE%\\n.txt"), QJsonValue()),
            entry(QStringLiteral("bad"), QStringLiteral("file"), QStringLiteral("C:relative.txt"), 9),
            entry(QStringLiteral("link"), QStringLiteral("shortcut"),
                  QDir(data).filePath(QStringLiteral("shortcuts/kept.lnk")), 2)};
        const QJsonObject root{{QStringLiteral("schemaVersion"), 1},
                               {QStringLiteral("tabs"),
                                QJsonArray{QJsonObject{{QStringLiteral("id"), LauncherCatalog::newId()},
                                                       {QStringLiteral("name"), QStringLiteral("开发")},
                                                       {QStringLiteral("items"), items}}}}};
        const auto write = [](const QString& path, const QJsonObject& o) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(o).toJson());
        };
        const auto itemsPath = QDir(data).filePath(QStringLiteral("items.json"));
        write(itemsPath, root);
        write(QDir(data).filePath(QStringLiteral("config.json")),
              {{QStringLiteral("schemaVersion"), 1},
               {QStringLiteral("appearance"), QJsonObject{{QStringLiteral("gridColumns"), 5}}}});
        const auto hash = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly)
                       ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256)
                       : QByteArray{};
        };
        const auto before = hash(itemsPath);
        // The EXE folder works as well as the data folder itself.
        auto imported = readHLaunchData(source.path(), target.path());
        QVERIFY2(imported, qPrintable(imported ? QString{} : imported.error()));
        QCOMPARE(hash(itemsPath), before);
        QCOMPARE(imported->skipped, 1); // the drive-relative path
        QCOMPARE(imported->categories.size(), 1);
        const auto& list = imported->categories[0].items;
        QCOMPARE(list.size(), 5);
        QHash<QString, QPair<int, LaunchItem>> byName;
        for (const auto& [slot, i] : list)
            byName.insert(i.name, {slot, i});
        // 5 columns -> 4: slot 6 is row 1, column 1; slot 4 is column 5 and overflows.
        QCOMPARE(byName[QStringLiteral("rel")].first, 5);
        QCOMPARE(byName[QStringLiteral("fifth")].first, 4);
        QCOMPARE(list.last().second.name, QStringLiteral("fifth"));
        QCOMPARE(byName[QStringLiteral("rel")].second.target,
                 QDir::toNativeSeparators(exe.filePath(QStringLiteral("tools/a.exe"))));
        QCOMPARE(byName[QStringLiteral("rel")].second.arguments,
                 QStringLiteral("--profile \"a b\""));
        QCOMPARE(byName[QStringLiteral("rel")].second.launchCount, 7);
        QCOMPARE(byName[QStringLiteral("web")].second.target, QStringLiteral("https://example.com"));
        QCOMPARE(byName[QStringLiteral("env")].second.target, QStringLiteral("%USERPROFILE%\\n.txt"));
        const auto copied = byName[QStringLiteral("link")].second.target;
        QVERIFY(QDir::fromNativeSeparators(copied).startsWith(target.path()));
        // Read the bytes: Qt resolves .lnk paths, and this fixture is not a real shortcut.
        const auto bytes = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
        };
        QCOMPARE(bytes(copied), QByteArray("shortcut bytes"));
        QCOMPARE(bytes(QDir(data).filePath(QStringLiteral("shortcuts/kept.lnk"))),
                 QByteArray("shortcut bytes"));

        // Merging fills the category of the same name and skips exact duplicates.
        auto c = LauncherCatalog::empty();
        const int development = c.categoryIndex(c.selectedCategory);
        QVERIFY(c.put(development, 5, item(QStringLiteral("occupied"))));
        int duplicates = 0;
        QCOMPARE(c.merge(imported->categories, &duplicates), 5);
        QCOMPARE(duplicates, 0);
        QCOMPARE(c.categories.size(), 4);
        QCOMPARE(c.items[c.categories[development].slots[6]].name, QStringLiteral("rel"));
        QVERIFY(c.validate());
        QCOMPARE(c.merge(imported->categories, &duplicates), 0);
        QCOMPARE(duplicates, 5);

        auto newer = root;
        newer[QStringLiteral("schemaVersion")] = 2;
        write(itemsPath, newer);
        QVERIFY(!readHLaunchData(data, target.path()));
        QVERIFY(!readHLaunchData(target.path(), target.path()));
    }
    void edgeZonesRespectCornersAndMonitorSeams() {
        // Two 100% monitors side by side and a 150% monitor to the right.
        const QList<EdgeMonitor> monitors{{QRect(0, 0, 1920, 1080), 1.0},
                                          {QRect(1920, 0, 1920, 1080), 1.0},
                                          {QRect(3840, 0, 2880, 1620), 1.5}};
        EdgeConfig config;
        config.zones = {EdgeZone::Left, EdgeZone::Right, EdgeZone::TopLeft};
        QCOMPARE(edgeZoneAt({0, 500}, monitors, config), EdgeZone::Left);
        QCOMPARE(edgeZoneAt({3, 500}, monitors, config), EdgeZone::Left);
        QCOMPARE(edgeZoneAt({4, 500}, monitors, config), EdgeZone::None);
        QCOMPARE(edgeZoneAt({0, 0}, monitors, config), EdgeZone::TopLeft);
        QCOMPARE(edgeZoneAt({0, 15}, monitors, config), EdgeZone::TopLeft);
        QCOMPARE(edgeZoneAt({0, 16}, monitors, config), EdgeZone::Left);
        // The seam between monitors is not an outer edge.
        QCOMPARE(edgeZoneAt({1919, 500}, monitors, config), EdgeZone::None);
        QCOMPARE(edgeZoneAt({1920, 500}, monitors, config), EdgeZone::None);
        // Below the shorter monitors the 150% monitor's left edge is exposed;
        // its thickness is 6 physical pixels.
        QCOMPARE(edgeZoneAt({3840, 1300}, monitors, config), EdgeZone::Left);
        QCOMPARE(edgeZoneAt({3845, 1300}, monitors, config), EdgeZone::Left);
        QCOMPARE(edgeZoneAt({3846, 1300}, monitors, config), EdgeZone::None);
        QCOMPARE(edgeZoneAt({6719, 100}, monitors, config), EdgeZone::Right);
        config.outerOnly = false;
        QCOMPARE(edgeZoneAt({1919, 500}, monitors, config), EdgeZone::Right);
        QCOMPARE(edgeZoneAt({1920, 500}, monitors, config), EdgeZone::Left);
        // Disabled zones never match, and corners fall back to an enabled edge.
        config.zones = {EdgeZone::Top};
        QCOMPARE(edgeZoneAt({0, 0}, monitors, config), EdgeZone::Top);
        QCOMPARE(edgeZoneAt({0, 500}, monitors, config), EdgeZone::None);
        QCOMPARE(edgeZoneFromKey(QStringLiteral("bottomRight")), EdgeZone::BottomRight);
        QCOMPARE(edgeZoneKey(EdgeZone::TopLeft), QStringLiteral("topLeft"));
        QCOMPARE(edgeZoneFromKey(QStringLiteral("middle")), EdgeZone::None);
    }
    void edgeDwellFiresOnceAndRearmsAfterLeaving() {
        EdgeDwell dwell;
        QCOMPARE(dwell.sample(EdgeZone::Left, 0), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 150), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 180), EdgeZone::Left);
        // Staying in the zone never fires again.
        QCOMPARE(dwell.sample(EdgeZone::Left, 1000), EdgeZone::None);
        // Leave and return before the cooldown ends: the dwell starts on the
        // sample where the cooldown expires, without another movement.
        dwell.reset();
        QCOMPARE(dwell.sample(EdgeZone::Left, 0), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 180), EdgeZone::Left);
        QCOMPARE(dwell.sample(EdgeZone::None, 210), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 240), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 280), EdgeZone::None); // cooldown over, dwell starts
        QCOMPARE(dwell.sample(EdgeZone::Left, 459), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Left, 460), EdgeZone::Left);
        // Switching zones restarts the dwell; suppression (None) cancels it.
        dwell.reset();
        QCOMPARE(dwell.sample(EdgeZone::Left, 0), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::TopLeft, 100), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::TopLeft, 200), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::None, 250), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::TopLeft, 300), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::TopLeft, 480), EdgeZone::TopLeft);
        // A clock that jumps backwards never fires early.
        dwell.reset();
        QCOMPARE(dwell.sample(EdgeZone::Right, 1000), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Right, 10), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Right, 100), EdgeZone::None);
        QCOMPARE(dwell.sample(EdgeZone::Right, 190), EdgeZone::Right);
    }
    void edgeProcessListsAndSettings() {
        const QStringList block{QStringLiteral("Game.exe")};
        QVERIFY(!edgeProcessAllowed(QStringLiteral("game.EXE"), block, {}));
        QVERIFY(edgeProcessAllowed(QStringLiteral("notepad.exe"), block, {}));
        QVERIFY(edgeProcessAllowed({}, block, {})); // unknown passes a block list
        const QStringList allow{QStringLiteral("game.exe")};
        QVERIFY(edgeProcessAllowed(QStringLiteral("GAME.exe"), block, allow)); // allow wins
        QVERIFY(!edgeProcessAllowed(QStringLiteral("notepad.exe"), {}, allow));
        QVERIFY(!edgeProcessAllowed({}, {}, allow)); // unknown fails an allow list
        QCOMPARE(parseProcessList(QStringLiteral(" a.exe\r\nb.exe;c.exe, A.EXE\n\n")),
                 (QStringList{QStringLiteral("a.exe"), QStringLiteral("b.exe"),
                              QStringLiteral("c.exe")}));
        auto s = Settings::defaults();
        QVERIFY(!s.launcherEdgeEnabled);
        s.launcherEdgeEnabled = true;
        s.launcherEdgeZones = {QStringLiteral("topLeft"), QStringLiteral("bottom")};
        s.launcherEdgeMode = QStringLiteral("every");
        s.launcherEdgeBlocklist = {QStringLiteral("game.exe")};
        const auto restored = Settings::decode(s.encode());
        QVERIFY(restored);
        QVERIFY(restored->launcherEdgeEnabled);
        QCOMPARE(restored->launcherEdgeZones, s.launcherEdgeZones);
        QCOMPARE(restored->launcherEdgeMode, QStringLiteral("every"));
        QCOMPARE(restored->launcherEdgeBlocklist, s.launcherEdgeBlocklist);
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherEdgeZones":["middle"]})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherEdgeZones":"left"})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherEdgeMode":"corner"})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherEdgeBlocklist":["C:\\game.exe"]})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherEdgeAllowlist":["*.exe"]})"));
    }
    void columnChangesKeepRowsAndColumns() {
        auto c = LauncherCatalog::empty();
        // 4 columns: a(0,0) b(0,3) d(1,2) e(2,1)
        QVERIFY(c.put(0, 0, item(QStringLiteral("a"))));
        QVERIFY(c.put(0, 3, item(QStringLiteral("b"))));
        QVERIFY(c.put(0, 6, item(QStringLiteral("d"))));
        QVERIFY(c.put(0, 9, item(QStringLiteral("e"))));
        const auto slots = c.categories[0].slots;
        const auto name = [&c](const QString& id) { return c.items.value(id).name; };
        QVERIFY(c.setColumns(6));
        QCOMPARE(c.columns, 6);
        auto& wide = c.categories[0].slots;
        QCOMPARE(name(wide[0]), QStringLiteral("a"));
        QCOMPARE(name(wide[3]), QStringLiteral("b"));
        QCOMPARE(name(wide[8]), QStringLiteral("d"));  // row 1, column 2
        QCOMPARE(name(wide[13]), QStringLiteral("e")); // row 2, column 1
        QVERIFY(c.validate());
        // Back to 3 columns: b (column 3) continues from the next row, skipping
        // the gap it left behind instead of filling earlier holes.
        QVERIFY(c.setColumns(3));
        auto& narrow = c.categories[0].slots;
        QCOMPARE(name(narrow[0]), QStringLiteral("a"));
        QCOMPARE(name(narrow[5]), QStringLiteral("d"));
        QCOMPARE(name(narrow[7]), QStringLiteral("e"));
        QCOMPARE(name(narrow[3]), QStringLiteral("b"));
        QCOMPARE(c.items.size(), 4);
        QVERIFY(c.validate());
        QVERIFY(!c.setColumns(2));
        QVERIFY(!c.setColumns(21));
        // The layout column count is saved with the slots; old files are 4 wide.
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("launcher.json"));
        QVERIFY(c.save(path));
        auto loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->columns, 3);
        QCOMPARE(loaded->categories[0].slots, c.categories[0].slots);
        auto legacy = LauncherCatalog::empty();
        QVERIFY(legacy.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        root.remove(QStringLiteral("columns"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        loaded = LauncherCatalog::load(path);
        QVERIFY(loaded);
        QCOMPARE(loaded->columns, 4);
        auto s = Settings::defaults();
        QCOMPARE(s.launcherColumns, 4);
        QCOMPARE(s.launcherRows, 7);
        s.launcherColumns = 8;
        s.launcherRows = 5;
        const auto restored = Settings::decode(s.encode());
        QVERIFY(restored);
        QCOMPARE(restored->launcherColumns, 8);
        QCOMPARE(restored->launcherRows, 5);
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherColumns":2})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"launcherRows":21})"));
    }
    void gestureIgnoresJitter() {
        GestureRecognizer r;
        r.begin({100, 100});
        r.append({102, 102});
        r.append({104, 100});
        QVERIFY(!r.hasStroke());
    }
    void gestureSegmentsAndReset() {
        GestureRecognizer r(20);
        r.begin({0, 0});
        r.append({0, 25});
        r.append({0, 50});
        r.append({30, 50});
        QCOMPARE(r.directions(), QStringLiteral("26"));
        r.begin({0, 0});
        r.append({30, 30});
        r.append({60, 0});
        QCOMPARE(r.directions(), QStringLiteral("39"));
        r.reset();
        QVERIFY(r.points().isEmpty());
    }
    void unlockPolicyRequiresExactPassword() {
        UnlockPolicy policy;
        QVERIFY(!policy.configure(true, QStringLiteral("中文")));
        QVERIFY(!policy.configure(true, {}));
        QVERIFY(policy.configure(true, QStringLiteral("Ab12")));
        for (QChar c : QStringLiteral("ab12"))
            policy.append(c);
        QVERIFY(!policy.submit());
        for (QChar c : QStringLiteral("Ab12"))
            policy.append(c);
        QVERIFY(policy.submit());
        QVERIFY(!policy.submit());
    }
    void simpleUnlockAndEditing() {
        UnlockPolicy policy;
        QVERIFY(policy.configure(false, {}));
        QVERIFY(policy.submit());
        QVERIFY(policy.configure(true, QStringLiteral("a1")));
        policy.append(u'a');
        policy.append(u'2');
        policy.backspace();
        policy.append(u'1');
        QVERIFY(policy.submit());
    }
    void lockInputModeAndRepeatRules() {
        LockInput input;
        const QKeySequence shortcut(QStringLiteral("Alt+L"));
        QVERIFY(input.begin(false, {}, shortcut));
        QCOMPARE(input.press(Qt::Key_L, Qt::AltModifier, u'l', true), LockInput::Result::Ignored);
        QCOMPARE(input.press(Qt::Key_L, Qt::AltModifier | Qt::ControlModifier, u'l', false), LockInput::Result::Ignored);
        QCOMPARE(input.press(Qt::Key_L, Qt::AltModifier, u'l', false), LockInput::Result::Unlock);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, true), LockInput::Result::Ignored);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Unlock);
        QVERIFY(input.begin(true, QStringLiteral("Ab12"), shortcut));
        QCOMPARE(input.press(Qt::Key_L, Qt::AltModifier, u'l', false), LockInput::Result::Ignored);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Rejected);
        for (QChar ch : QStringLiteral("ab12"))
            input.press(ch.toUpper().unicode(), {}, ch, false);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Rejected);
        for (QChar ch : QStringLiteral("Ab12"))
            input.press(ch.toUpper().unicode(), ch.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, ch, false);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Unlock);
    }
    void lockInputClearsWrongAttemptsAndEdits() {
        LockInput input;
        QVERIFY(input.begin(true, QStringLiteral("a1"), QKeySequence(QStringLiteral("Alt+L"))));
        input.press(Qt::Key_A, {}, u'a', false);
        input.press(Qt::Key_Escape, {}, {}, false);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Rejected);
        input.press(Qt::Key_A, {}, u'a', false);
        input.press(Qt::Key_2, {}, u'2', false);
        input.press(Qt::Key_Backspace, {}, {}, false);
        input.press(Qt::Key_1, {}, u'1', false);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Unlock);
        QCOMPARE(input.press(Qt::Key_Return, {}, {}, false), LockInput::Result::Rejected);
    }
    void lockSettingsPersistModeAndPause() {
        auto settings = Settings::defaults();
        settings.lockEnabled = true;
        settings.lockPaused = true;
        settings.requirePassword = true;
        QVERIFY(!settings.validate());
        settings.protectedPassword = QStringLiteral("dpapi:test-fixture");
        const auto loaded = Settings::decode(settings.encode());
        QVERIFY(loaded);
        QVERIFY(loaded->lockEnabled && loaded->lockPaused && loaded->requirePassword);
        QCOMPARE(loaded->protectedPassword, settings.protectedPassword);
        settings.lockHotkey = settings.clipboardHotkey;
        QVERIFY(!settings.validate());
    }
};
QTEST_GUILESS_MAIN(BehaviorTests)
#include "BehaviorTests.moc"
