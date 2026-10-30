#include "infrastructure/settings/Settings.h"
#include "platform/windows/AdminBridge.h"
#include "platform/windows/StartupRegistration.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <wil/resource.h>
#include <windows.h>
using namespace mt;
class SystemIntegrationTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void settingsAreOptInAndRejectWrongTypes() {
        auto original = Settings::decode(QByteArrayLiteral("{\"schema\":1}"));
        QVERIFY(original);
        QVERIFY(!original->autoStart && !original->adminSupport);
        original->autoStart = original->adminSupport = true;
        auto loaded = Settings::decode(original->encode());
        QVERIFY(loaded);
        QVERIFY(loaded->autoStart && loaded->adminSupport);
        QVERIFY(!Settings::decode(QByteArrayLiteral("{\"schema\":1,\"autoStart\":1}")));
        QVERIFY(!Settings::decode(QByteArrayLiteral("{\"schema\":1,\"adminSupport\":\"yes\"}")));
    }
    void startupRegistrationPreservesOtherEntriesAndCanRollback() {
        const auto key = QStringLiteral("Software\\MT.Tests\\") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto cleanup =
            qScopeGuard([&] { RegDeleteTreeW(HKEY_CURRENT_USER, key.toStdWString().c_str()); });
        QSettings other(QStringLiteral("HKEY_CURRENT_USER\\") + key, QSettings::NativeFormat);
        other.setValue(QStringLiteral("Unrelated"), QStringLiteral("keep"));
        other.sync();
        StartupRegistration startup(QStringLiteral("C:/MT Test/mt.exe"),
                                    QStringLiteral("C:/用户 数据/"), key);
        auto initial = startup.read();
        QVERIFY(initial);
        QVERIFY(!*initial);
        QVERIFY(startup.setEnabled(true));
        auto enabled = startup.read();
        QVERIFY(enabled);
        QCOMPARE(**enabled, startup.command());
        QVERIFY(
            startup.command().startsWith(QStringLiteral("\"C:\\MT Test\\mt.exe\" --data-dir \"")));
        QVERIFY(startup.restore(*initial, *enabled));
        QVERIFY(!*startup.read());
        QVERIFY(startup.setEnabled(true));
        QVERIFY(startup.setEnabled(false));
        QVERIFY(!*startup.read());
        QCOMPARE(other.value(QStringLiteral("Unrelated")).toString(), QStringLiteral("keep"));
        other.setValue(QStringLiteral("MT"), QStringLiteral("foreign.exe"));
        other.sync();
        QVERIFY(!startup.setEnabled(true));
        QVERIFY(!startup.setEnabled(false));
        QVERIFY(!startup.restore({}, StartupRegistration::Snapshot{startup.command()}));
        QCOMPARE(**startup.read(), QStringLiteral("foreign.exe"));
    }
    void startupRejectsUnusablePathsBeforeWriting() {
        const auto key = QStringLiteral("Software\\MT.Tests\\") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto cleanup =
            qScopeGuard([&] { RegDeleteTreeW(HKEY_CURRENT_USER, key.toStdWString().c_str()); });
        StartupRegistration longPath(QStringLiteral("C:/mt.exe"),
                                     QStringLiteral("C:/") + QString(300, u'x'), key);
        QVERIFY(!longPath.setEnabled(true));
        QVERIFY(!*longPath.read());
        StartupRegistration quote(QStringLiteral("C:/bad\"/mt.exe"), QStringLiteral("C:/data"),
                                  key);
        QVERIFY(!quote.setEnabled(true));
        QVERIFY(!*quote.read());
    }
    void authenticatedHelperRoundTripAndPreviewCannotInject() {
        AdminBridge bridge;
        bool completed = false, ok = false;
        QString error;
        bridge.start(
            [&](auto result) {
                completed = true;
                ok = bool(result);
                if (!result)
                    error = result.error();
            },
            true);
        QTRY_VERIFY_WITH_TIMEOUT(completed, 12000);
        QVERIFY2(ok, qPrintable(error));
        QVERIFY(bridge.ready());
        completed = false;
        bridge.probe(this, [&](auto result) {
            completed = true;
            ok = bool(result);
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 3000);
        QVERIFY(ok);
        completed = false;
        bridge.paste({}, this, [&](auto result) {
            completed = true;
            ok = bool(result);
            if (!result)
                error = result.error();
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 3000);
        QVERIFY(!ok);
        QVERIFY(error.contains(QStringLiteral("隔离验证")));
        QSignalSpy failed(&bridge, &AdminBridge::failed);
        bridge.replayClick({}, 0, {}, QStringLiteral("right"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QVERIFY(failed.front().front().toString().contains(QStringLiteral("隔离验证")));
        QVERIFY(bridge.stop());
        QVERIFY(!bridge.ready());
        QVERIFY(!bridge.busy());
    }
    void helperCompletionDoesNotOutliveReceiver() {
        AdminBridge bridge;
        bool connected = false;
        bridge.start([&](auto result) { connected = bool(result); }, true);
        QTRY_VERIFY_WITH_TIMEOUT(connected, 12000);
        auto receiver = std::make_unique<QObject>();
        bool called = false;
        bridge.probe(receiver.get(), [&](auto) { called = true; });
        receiver.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!bridge.busy(), 3000);
        QVERIFY(!called);
        QVERIFY(bridge.stop());
    }
    void helperExitsWhenParentIsTerminated() {
        QProcess owner;
        owner.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--admin-owner-fixture")});
        QVERIFY(owner.waitForReadyRead(5000));
        const auto pid = owner.readAllStandardOutput().trimmed().toUInt();
        QVERIFY(pid);
        wil::unique_handle helper(OpenProcess(SYNCHRONIZE, FALSE, pid));
        QVERIFY(helper);
        owner.kill();
        QVERIFY(owner.waitForFinished(5000));
        QCOMPARE(WaitForSingleObject(helper.get(), 4000), DWORD(WAIT_OBJECT_0));
    }
    void helperRejectsWrongParentCreationTime() {
        QProcess child;
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--admin-helper"),
             QStringLiteral("mt-admin-") + QUuid::createUuid().toString(QUuid::WithoutBraces),
             QStringLiteral("--admin-parent"), QString::number(GetCurrentProcessId()),
             QStringLiteral("--admin-started"), QString::number(currentProcessStarted() + 1),
             QStringLiteral("--preview")});
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 3);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto value = [&](const QString& key) {
        const auto index = args.indexOf(key);
        return index < 0 ? QString{} : args.value(index + 1);
    };
    if (args.contains(QStringLiteral("--admin-owner-fixture"))) {
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const auto pipe =
            QStringLiteral("mt-admin-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!server.listen(pipe))
            return 20;
        qint64 pid = 0;
        if (!QProcess::startDetached(
                app.applicationFilePath(),
                {QStringLiteral("--admin-helper"), pipe, QStringLiteral("--admin-parent"),
                 QString::number(GetCurrentProcessId()), QStringLiteral("--admin-started"),
                 QString::number(currentProcessStarted()), QStringLiteral("--preview")},
                {}, &pid))
            return 21;
        if (!server.waitForNewConnection(5000))
            return 22;
        auto* socket = server.nextPendingConnection();
        if (!socket || (!socket->bytesAvailable() && !socket->waitForReadyRead(5000)))
            return 23;
        QTextStream output(stdout);
        output << pid << Qt::endl;
        QTimer::singleShot(10000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (args.contains(QStringLiteral("--admin-helper")))
        return runAdminHelper(value(QStringLiteral("--admin-helper")),
                              value(QStringLiteral("--admin-parent")).toUInt(),
                              value(QStringLiteral("--admin-started")).toULongLong(),
                              args.contains(QStringLiteral("--preview")));
    SystemIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "SystemIntegrationTests.moc"
