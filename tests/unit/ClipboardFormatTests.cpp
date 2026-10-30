#include "infrastructure/storage/ClipboardStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtPlugin>
#include <QtTest>
Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)
using namespace mt;
class ClipboardFormatTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void richFormatsSurviveDatabaseAndPlainTextStripsThem() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("clips.sqlite"));
        QMimeData source;
        source.setText(QStringLiteral("你好"));
        source.setHtml(QStringLiteral("<b>你好</b>"));
        const QByteArray rtf("{\\rtf1\\ansi\\b hello}");
        const QByteArray binary("\0\x01\x02\xff", 4);
        source.setData(QStringLiteral("text/rtf"), rtf);
        source.setData(QStringLiteral("application/x-example-sheet"), binary);
        auto record = ClipboardRecord::snapshot(source);
        QVERIFY(record);
        record->finalize();
        {
            ClipboardStore store(path);
            QVERIFY(store.open());
            QVERIFY(store.capture(*record, 100));
        }
        ClipboardStore reopened(path);
        QVERIFY(reopened.open());
        const auto loaded = reopened.record(record->id);
        QVERIFY(loaded);
        const auto normal = loaded->mime();
        QCOMPARE(normal->text(), source.text());
        QCOMPARE(normal->html(), source.html());
        QCOMPARE(normal->data(QStringLiteral("text/rtf")), rtf);
        QCOMPARE(normal->data(QStringLiteral("application/x-example-sheet")), binary);
        const auto plain = loaded->mime(true);
        QCOMPARE(plain->formats(), QStringList{QStringLiteral("text/plain")});
        QCOMPARE(plain->text(), source.text());
        auto listed = reopened.records({}, 0, 20);
        QVERIFY(listed);
        QVERIFY(listed->front().formats.isEmpty()); // large payload remains demand-loaded
    }
    void rtfOnlyAndCustomOnlyDoNotInventPlainText() {
        for (const auto& format :
             {QStringLiteral("text/rtf"),
              QStringLiteral("application/x-qt-windows-mime;value=\"Rich Text Format\""),
              QStringLiteral("application/x-private-fixture")}) {
            QMimeData source;
            source.setData(format, QByteArrayLiteral("{\\rtf1 content}"));
            auto record = ClipboardRecord::snapshot(source);
            QVERIFY(record);
            QCOMPARE(record->kind, ClipKind::Formats);
            QVERIFY(!record->summary().isEmpty());
            QCOMPARE(record->mime()->data(format), source.data(format));
            QVERIFY(!record->mime()->hasText());
            QVERIFY_THROWS_EXCEPTION(std::logic_error, record->mime(true));
        }
    }
    void parallelImageHtmlAndFileRepresentationsSurvive() {
        QTemporaryDir dir;
        ClipboardStore store(dir.filePath(QStringLiteral("parallel.sqlite")));
        QVERIFY(store.open());
        QImage image(12, 9, QImage::Format_ARGB32);
        image.fill(Qt::green);
        for (const bool withFiles : {false, true}) {
            QMimeData source;
            source.setHtml(QStringLiteral("<img src='fixture.png'>"));
            source.setImageData(image);
            if (withFiles)
                source.setUrls({QUrl::fromLocalFile(dir.filePath(QStringLiteral("示例.png")))});
            auto record = ClipboardRecord::snapshot(source);
            QVERIFY(record);
            record->finalize();
            QCOMPARE(record->kind, withFiles ? ClipKind::Files : ClipKind::Image);
            QVERIFY(store.capture(*record, 100));
            auto loaded = store.record(record->id);
            QVERIFY(loaded);
            auto restored = loaded->mime();
            QCOMPARE(restored->html(), source.html());
            QCOMPARE(qvariant_cast<QImage>(restored->imageData()), image);
            QCOMPARE(restored->urls(), source.urls());
        }
    }
    void formattingParticipatesInDeduplication() {
        QMimeData source;
        source.setText(QStringLiteral("same visible text"));
        source.setData(QStringLiteral("text/rtf"), QByteArrayLiteral("{\\rtf1\\b hello}"));
        auto bold = ClipboardRecord::snapshot(source);
        QVERIFY(bold);
        bold->finalize();
        source.setData(QStringLiteral("text/rtf"), QByteArrayLiteral("{\\rtf1\\i hello}"));
        auto italic = ClipboardRecord::snapshot(source);
        QVERIFY(italic);
        italic->finalize();
        QVERIFY(bold->id != italic->id);
        auto again = ClipboardRecord::snapshot(source);
        again->finalize();
        QCOMPARE(again->id, italic->id);
    }
    void unsafeMetadataAndOversizedPayloadsAreNotReplayed() {
        QMimeData source;
        source.setText(QStringLiteral("text"));
        source.setData(QStringLiteral("application/x-mt-writeback"),
                       QByteArrayLiteral("old-process"));
        source.setData(QStringLiteral("application/x-qt-windows-mime;value=\"Ole Private Data\""),
                       QByteArrayLiteral("handle"));
        source.setData(
            QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
            QByteArrayLiteral("move"));
        auto record = ClipboardRecord::snapshot(source);
        QVERIFY(record);
        QVERIFY(record->formats.isEmpty());
        source.setData(QStringLiteral("text/rtf"),
                       QByteArray(ClipboardRecord::maxFormatBytes + 1, 'x'));
        QVERIFY(!ClipboardRecord::snapshot(source));
        ClipboardRecord corrupted;
        QVERIFY(!corrupted.decodeFormats(QByteArrayLiteral("{\"text/rtf\":\"not base64!\"}")));
        QVERIFY(!corrupted.decodeFormats(QByteArrayLiteral("[]")));
    }
    void schemaOneUpgradeKeepsRecordsAndCreatesReadableBackup() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("old.sqlite"));
        const auto name = QStringLiteral("format-upgrade-fixture");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery q(database);
            QVERIFY(q.exec(
                QStringLiteral("CREATE TABLE clips(id TEXT PRIMARY KEY,kind INTEGER,text TEXT,html "
                               "TEXT,files TEXT,image BLOB,source TEXT,captured INTEGER,favorite "
                               "INTEGER,pinned INTEGER,sensitive INTEGER,thumbnail BLOB)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO clips VALUES('original',0,'保留','', '[]',NULL,'',1,1,1,1,NULL)")));
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version=1")));
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        ClipboardStore store(path);
        QVERIFY(store.open());
        const auto record = store.record(QStringLiteral("original"));
        QVERIFY(record);
        QCOMPARE(record->text, QStringLiteral("保留"));
        QVERIFY(record->favorite && record->pinned && record->sensitive);
        const auto backups = QDir(dir.path()).entryList({QStringLiteral("*.bak")});
        QCOMPARE(backups.size(), 1);
        {
            auto backup = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            backup.setDatabaseName(dir.filePath(backups.front()));
            backup.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            QVERIFY(backup.open());
            QSqlQuery q(backup);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            QVERIFY(q.exec(QStringLiteral("SELECT text FROM clips WHERE id='original'")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), record->text);
            backup.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    void unversionedExistingDatabaseIsNotInitialized() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("unknown.sqlite"));
        const auto name = QStringLiteral("unversioned-format-fixture");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery q(database);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE unrelated(value TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO unrelated VALUES('keep')")));
        }
        QSqlDatabase::removeDatabase(name);
        {
            ClipboardStore store(path);
            QVERIFY(!store.open());
        }
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery q(database);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);
            QVERIFY(q.exec(QStringLiteral("SELECT value FROM unrelated")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("keep"));
            QVERIFY(q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE name='clips'")));
            QVERIFY(!q.next());
        }
        QSqlDatabase::removeDatabase(name);
    }
};
QTEST_GUILESS_MAIN(ClipboardFormatTests)
#include "ClipboardFormatTests.moc"
