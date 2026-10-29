// Explicit desktop tests: deliberately excluded from ctest. Run only via the
// bounded PowerShell harness, with no user typing while these tests are running.
#include "modules/screen_lock/ScreenLockService.h"
#include "app/Application.h"
#include "platform/windows/LockPassword.h"
#include <QApplication>
#include <QLineEdit>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtPlugin>
#include <QtTest>
#include <memory>
#include <windows.h>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)
using namespace mt;
class ScreenLockTests final : public QObject {
    Q_OBJECT
    std::unique_ptr<QLineEdit> sink_;
    POINT originalCursor_{};
    static void key(WORD vk, bool down) {
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = vk;
        event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(1, &event, sizeof(event)), UINT(1));
    }
    static void tap(WORD vk) {
        key(vk, true);
        key(vk, false);
    }
    static void focusSink(QWidget* widget) {
        const auto hwnd = reinterpret_cast<HWND>(widget->winId());
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT point{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(hwnd, &point);
        SetCursorPos(point.x, point.y);
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        QCOMPARE(SendInput(2, click, sizeof(INPUT)), UINT(2));
        QTRY_VERIFY_WITH_TIMEOUT(GetForegroundWindow() == hwnd && GetFocus() == hwnd, 2000);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    static void chord() {
        key(VK_LMENU, true);
        tap('L');
        key(VK_LMENU, false);
    }
    static void recoveryChord() {
        key(VK_LCONTROL, true);
        key(VK_LMENU, true);
        key(VK_LSHIFT, true);
        tap(VK_F9);
        key(VK_LSHIFT, false);
        key(VK_LMENU, false);
        key(VK_LCONTROL, false);
    }
    static bool visibleBlackout() {
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->objectName() == QStringLiteral("mtBlackout") && widget->isVisible())
                return true;
        return false;
    }
    static void password() {
        key(VK_LSHIFT, true);
        tap('A');
        key(VK_LSHIFT, false);
        tap('B');
        tap('1');
        tap('2');
        tap(VK_RETURN);
    }
  private Q_SLOTS:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("MT_RUN_LOCK_INTEGRATION") == QStringLiteral("1"),
                 "Use scripts/verify-screen-lock.ps1; this suite captures desktop input.");
        qApp->setQuitOnLastWindowClosed(false);
        GetCursorPos(&originalCursor_);
        sink_ = std::make_unique<QLineEdit>();
        sink_->setWindowTitle(QStringLiteral("mt isolated blackout test"));
        sink_->resize(400, 80);
        sink_->show();
        sink_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(sink_.get()));
        focusSink(sink_.get());
        // Synthetic fixtures use Caps Lock off, restoring it in cleanupTestCase.
        capsInitially_ = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (capsInitially_)
            tap(VK_CAPITAL);
    }
    void cleanupTestCase() {
        if (capsInitially_ != ((GetKeyState(VK_CAPITAL) & 1) != 0))
            tap(VK_CAPITAL);
        sink_.reset();
        SetCursorPos(originalCursor_.x, originalCursor_.y);
    }
    void simpleEnterAndScreenCoverage() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopServices probe(root.path());
        QVERIFY(probe.valid() && probe.primaryInstance());
        QVERIFY(probe.setHotkeys({{91, QStringLiteral("Ctrl+Alt+Shift+F9")}}));
        QSignalSpy hotkeys(&probe, &DesktopServices::hotkey);
        // Establish the same native input route before and after capture; this
        // checks WM_HOTKEY delivery without depending on Qt text focus or an IME.
        recoveryChord();
        QTRY_COMPARE_WITH_TIMEOUT(hotkeys.count(), 1, 2000);
        ScreenLockService service;
        QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        int windows = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() != QStringLiteral("mtBlackout"))
                continue;
            ++windows;
            QVERIFY(widget->isVisible());
            QCOMPARE(widget->geometry(), widget->screen()->geometry());
            QCOMPARE(widget->cursor().shape(), Qt::BlankCursor);
            QTest::mouseClick(widget, Qt::LeftButton);
            QVERIFY(service.active());
            const auto image = widget->grab().toImage();
            QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2), QColor(Qt::black));
        }
        QCOMPARE(windows, QGuiApplication::screens().size());
        recoveryChord();
        QTest::qWait(100);
        QCOMPARE(hotkeys.count(), 1); // suppressed while the blackout owns input
        tap(VK_RETURN);
        QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
        recoveryChord();
        QTRY_COMPARE_WITH_TIMEOUT(hotkeys.count(), 2, 2000);
    }
    void heldActivationDoesNotImmediatelyUnlock() {
        sink_->activateWindow();
        key(VK_LMENU, true);
        key('L', true);
        ScreenLockService service;
        QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        key('L', true); // auto-repeat from the original lock chord
        QTest::qWait(150);
        QVERIFY(service.active());
        key('L', false);
        key(VK_LMENU, false);
        QTest::qWait(150);
        QVERIFY(service.active());
        chord();
        QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
    }
    void unrelatedHeldKeyDoesNotBlockEnter() {
        key('Z', true);
        ScreenLockService service;
        QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        tap(VK_RETURN);
        key('Z', false);
        QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
    }
    void passwordRequiresCorrectCaseAndSurvivesHookRenewal() {
        const auto secret = protectLockPassword(QStringLiteral("Ab12"));
        QVERIFY(secret);
        ScreenLockService service;
        QVERIFY(service.start(true, *secret, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        chord();
        tap(VK_RETURN);
        QTest::qWait(150);
        QVERIFY(service.active());
        tap('A');
        tap('B');
        tap('1');
        tap('2');
        tap(VK_RETURN);
        QTest::qWait(150);
        QVERIFY(service.active());
        // Exercise the renewal path; this is not an hours-long soak test.
        QTest::qWait(5500);
        password();
        QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
    }
    void invalidPasswordCannotAcquireInput() {
        ScreenLockService service;
        QVERIFY(!service.start(true, QStringLiteral("dpapi:invalid"), QStringLiteral("Alt+L")));
        QVERIFY(!service.active());
        QVERIFY(!service.start(true, {}, QStringLiteral("Alt+L")));
        QVERIFY(!service.active());
    }
    void guiStallReleasesInputLease() {
        ScreenLockService service;
        QSignalSpy failed(&service, &ScreenLockService::failed);
        QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        Sleep(4500); // intentionally stop GUI pulses, the hook thread must release
        QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
        QCOMPARE(failed.count(), 1);
    }
    void stoppingAndRestartingRestoresInput() {
        ScreenLockService service;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
            QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
            service.stop();
            QTRY_VERIFY_WITH_TIMEOUT(!service.active(), 2000);
        }
        QVERIFY(service.start(false, {}, QStringLiteral("Alt+L")));
        QTRY_VERIFY_WITH_TIMEOUT(service.locked(), 2000);
        service.shutdown();
        QVERIFY(!service.active());
    }
    void hostHotkeyGatesPanelsAndShutdownCleansUp() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto settings = Settings::defaults();
        // No system clipboard acquisition, real launcher targets or old data.
        settings.clipboardEnabled = false;
        settings.launcherEnabled = false;
        settings.lockEnabled = true;
        settings.lockHotkey = QStringLiteral("Ctrl+Alt+Shift+F10");
        SettingsStore store(root.path());
        QVERIFY(store.save(settings));
        DesktopServices desktop(root.path());
        QVERIFY(desktop.valid() && desktop.primaryInstance());
        {
            Application host(root.path(), false, desktop);
            QVERIFY(host.start());
            QTest::qWait(250);
            key(VK_LCONTROL, true); key(VK_LMENU, true); key(VK_LSHIFT, true);
            tap(VK_F10);
            key(VK_LSHIFT, false); key(VK_LMENU, false); key(VK_LCONTROL, false);
            QTRY_VERIFY_WITH_TIMEOUT(visibleBlackout(), 2000);
            host.showSettings();
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->isVisible())
                    QVERIFY(widget->objectName() == QStringLiteral("mtBlackout") || widget == sink_.get());
            tap(VK_RETURN);
            QTRY_VERIFY_WITH_TIMEOUT(!visibleBlackout(), 2000);
            host.lockScreen();
            QTRY_VERIFY_WITH_TIMEOUT(visibleBlackout(), 2000);
            // Host destruction, including input and storage threads, must complete.
        }
        QVERIFY(!visibleBlackout());
        const auto loaded = store.load();
        QVERIFY(loaded);
        QVERIFY(loaded->lockEnabled && !loaded->clipboardEnabled && !loaded->launcherEnabled);
    }

  private:
    bool capsInitially_ = false;
};
QTEST_MAIN(ScreenLockTests)
#include "ScreenLockTests.moc"
