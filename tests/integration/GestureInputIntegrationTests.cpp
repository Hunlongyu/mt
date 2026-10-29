#include <windows.h>
#include "platform/windows/GestureInput.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QMouseEvent>
#include <QProcess>
#include <QSignalSpy>
#include <QtTest>
#include <array>
#include <iostream>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
using namespace mt;

class InputTarget final : public QWidget {
    int downs_ = 0, ups_ = 0, contexts_ = 0;
  protected:
    bool nativeEvent(const QByteArray&, void* raw, qintptr* result) override {
        const auto* message = static_cast<MSG*>(raw);
        if (message->message == WM_RBUTTONDOWN || message->message == WM_RBUTTONDBLCLK) ++downs_;
        if (message->message == WM_RBUTTONUP) ++ups_;
        if (message->message != WM_APP + 71) return false;
        *result = message->wParam == 1 ? downs_ : message->wParam == 2 ? ups_ : contexts_;
        return true;
    }
    void contextMenuEvent(QContextMenuEvent* event) override { ++contexts_; event->accept(); }
};
class GestureInputIntegrationTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void pressAdmissionDoesNotDependOnPolling_data() {
        QTest::addColumn<bool>("changedWindow");
        QTest::newRow("move-then-immediate-press") << true;
        QTest::newRow("release-then-immediate-repress") << false;
    }
    void pressAdmissionDoesNotDependOnPolling() {
        if (!qEnvironmentVariableIsSet("MT_GESTURE_DESKTOP_TEST"))
            QSKIP("Opt-in Windows input test, isolated target process only");
        QFETCH(bool, changedWindow);
        const auto previous = GetForegroundWindow();
        POINT cursor{};
        GetCursorPos(&cursor);
        struct Restore {
            HWND previous; POINT cursor;
            ~Restore() { SetCursorPos(cursor.x, cursor.y); if (IsWindow(previous)) SetForegroundWindow(previous); }
        } restore{previous, cursor};
        QProcess target;
        target.setProgram(QCoreApplication::applicationFilePath());
        target.setArguments({QStringLiteral("--gesture-target")});
        target.start();
        QVERIFY(target.waitForStarted());
        QVERIFY(target.waitForReadyRead(5000));
        bool ok = false;
        const auto window = reinterpret_cast<HWND>(quintptr(target.readLine().trimmed().toULongLong(&ok, 16)));
        QVERIFY(ok && IsWindow(window));
        struct CloseTarget {
            QProcess& process; HWND window;
            ~CloseTarget() { PostMessageW(window, WM_CLOSE, 0, 0); if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(); } }
        } close{target, window};
        const auto foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = foregroundThread && foregroundThread != GetCurrentThreadId() &&
            AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        if (attached) AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
        QTRY_COMPARE(GetForegroundWindow(), window);
        const auto movement = [](POINT point) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                               MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
            input.mi.dx = LONG(((qint64(point.x) - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 2 + 1) * 65536 /
                               (GetSystemMetrics(SM_CXVIRTUALSCREEN) * 2));
            input.mi.dy = LONG(((qint64(point.y) - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 2 + 1) * 65536 /
                               (GetSystemMetrics(SM_CYVIRTUALSCREEN) * 2));
            return input;
        };
        const auto button = [](DWORD flags) {
            INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = flags; return input;
        };
        POINT start{200, 180};
        QVERIFY(ClientToScreen(window, &start));
        const POINT end{start.x + 80, start.y};
        QWidget ownWindow(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        ownWindow.setAttribute(Qt::WA_ShowWithoutActivating);
        ownWindow.setGeometry(130, 130, 80, 80);
        ownWindow.show();
        QTest::qWait(30);
        RECT ownBounds{};
        QVERIFY(GetWindowRect(reinterpret_cast<HWND>(ownWindow.winId()), &ownBounds));
        const POINT ownPoint{(ownBounds.left + ownBounds.right) / 2, (ownBounds.top + ownBounds.bottom) / 2};
        QVERIFY(GetAncestor(WindowFromPoint(start), GA_ROOTOWNER) == window);
        QVERIFY(GetAncestor(WindowFromPoint(end), GA_ROOTOWNER) == window);
        GestureInput input;
        GestureInputOptions options;
        options.enabled = true;
        options.suppressFullscreen = false;
        auto rules = GestureRules::parse({{QStringLiteral("6"), QStringLiteral("key:F6")}}, {}, {}, {});
        QVERIFY(rules.has_value());
        options.rules = *rules;
        QSignalSpy frames(&input, &GestureInput::frame);
        QSignalSpy failures(&input, &GestureInput::failed);
        QVERIFY(input.configure(options));
        const auto finished = [&] {
            int count = 0;
            for (const auto& arguments : frames) {
                const auto frame = qvariant_cast<GestureFrame>(arguments.first());
                if (frame.finished && frame.drawing && !frame.cancelled) ++count;
            }
            return count;
        };
        const auto received = [window](int kind) {
            DWORD_PTR value = 0;
            if (!SendMessageTimeoutW(window, WM_APP + 71, WPARAM(kind), 0, SMTO_ABORTIFHUNG, 1000, &value)) return -1;
            return int(value);
        };
        const int strokes = changedWindow ? 1 : 2;
        for (int attempt = 0; attempt < 24; ++attempt) {
            QCOMPARE(GetForegroundWindow(), window);
            QCOMPARE(GetAncestor(WindowFromPoint(start), GA_ROOTOWNER), window);
            QCOMPARE(GetAncestor(WindowFromPoint(end), GA_ROOTOWNER), window);
            auto position = movement(changedWindow ? ownPoint : start);
            QCOMPARE(SendInput(1, &position, sizeof(INPUT)), 1u);
            QTest::qWait(30); // deliberately warm a different window's gate
            std::array<INPUT, 8> events{};
            for (int i = 0; i < strokes; ++i) {
                events[size_t(i) * 4] = movement(start);
                events[size_t(i) * 4 + 1] = button(MOUSEEVENTF_RIGHTDOWN);
                events[size_t(i) * 4 + 2] = movement(end);
                events[size_t(i) * 4 + 3] = button(MOUSEEVENTF_RIGHTUP);
            }
            // No sleep/polling interval between entering, pressing, moving,
            // releasing and pressing again. The real hook must own every pair.
            QCOMPARE(SendInput(UINT(strokes * 4), events.data(), sizeof(INPUT)), UINT(strokes * 4));
            QTest::qWait(60); // drain delivery only AFTER the uninterrupted input burst
            QCOMPARE(received(1), 0);
            QCOMPARE(received(2), 0);
            QCOMPARE(received(4), 0);
            QTRY_COMPARE_WITH_TIMEOUT(finished(), (attempt + 1) * strokes, 500);
        }
        // Cold admission still respects exclusions; it must not swallow a
        // native drag merely because cached metadata belongs to another window.
        auto excluded = GestureRules::parse({}, {},
            {QFileInfo(QCoreApplication::applicationFilePath()).fileName()}, {});
        QVERIFY(excluded.has_value());
        options.rules = *excluded;
        QVERIFY(input.configure(options));
        auto position = movement(ownPoint);
        QCOMPARE(SendInput(1, &position, sizeof(INPUT)), 1u);
        QTest::qWait(30);
        QCOMPARE(GetForegroundWindow(), window);
        QCOMPARE(GetAncestor(WindowFromPoint(start), GA_ROOTOWNER), window);
        QCOMPARE(GetAncestor(WindowFromPoint(end), GA_ROOTOWNER), window);
        std::array<INPUT, 4> bypass{movement(start), button(MOUSEEVENTF_RIGHTDOWN),
                                    movement(end), button(MOUSEEVENTF_RIGHTUP)};
        QCOMPARE(SendInput(UINT(bypass.size()), bypass.data(), sizeof(INPUT)), UINT(bypass.size()));
        QTRY_COMPARE(received(1), 1);
        QTRY_COMPARE(received(2), 1);
        QCOMPARE(finished(), 24 * strokes);
        // A stationary click on an admitted new target is replayed exactly once.
        options.rules = *rules;
        QVERIFY(input.configure(options));
        QCOMPARE(SendInput(1, &position, sizeof(INPUT)), 1u);
        QTest::qWait(30);
        QCOMPARE(GetForegroundWindow(), window);
        QCOMPARE(GetAncestor(WindowFromPoint(start), GA_ROOTOWNER), window);
        const auto menus = received(4);
        std::array<INPUT, 3> click{movement(start), button(MOUSEEVENTF_RIGHTDOWN), button(MOUSEEVENTF_RIGHTUP)};
        QCOMPARE(SendInput(UINT(click.size()), click.data(), sizeof(INPUT)), UINT(click.size()));
        QTRY_COMPARE(received(1), 2);
        QTRY_COMPARE(received(2), 2);
        QTRY_COMPARE(received(4), menus + 1);
        QCOMPARE(finished(), 24 * strokes);
        QVERIFY(failures.isEmpty());
    }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--gesture-target"))) {
        InputTarget target;
        target.setWindowTitle(QStringLiteral("mt isolated gesture input target"));
        target.setGeometry(120, 120, 720, 440);
        target.show();
        target.activateWindow();
        std::cout << std::hex << quintptr(target.winId()) << std::endl;
        return app.exec();
    }
    app.setQuitOnLastWindowClosed(false);
    GestureInputIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GestureInputIntegrationTests.moc"
