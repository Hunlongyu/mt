#include <windows.h>
#include "app/GestureController.h"
#include "infrastructure/i18n/I18n.h"
#include "infrastructure/settings/Settings.h"
#include "modules/gestures/GestureCapture.h"
#include "modules/gestures/GestureOverlay.h"
#include "modules/gestures/GestureRecognizer.h"
#include "modules/gestures/GestureRules.h"
#include "platform/windows/GestureActions.h"
#include "platform/windows/InputTag.h"
#include "ui/components/Theme.h"
#include "ui/shell/GestureSettings.h"
#include <QApplication>
#include <QBackingStore>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPointer>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
using namespace mt;

namespace {
QImage backingPixels(const QWidget& widget) {
    auto* store = widget.backingStore();
    if (!store) return {};
    const auto* image = dynamic_cast<const QImage*>(store->paintDevice());
    return image ? image->copy() : QImage{};
}
int visiblePixels(const QImage& image, QRect logicalArea) {
    const auto scale = image.devicePixelRatio();
    const QRect pixels(qFloor(logicalArea.x() * scale), qFloor(logicalArea.y() * scale),
                       qCeil(logicalArea.width() * scale), qCeil(logicalArea.height() * scale));
    const auto area = pixels.intersected(image.rect());
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (qAlpha(image.pixel(x, y))) ++count;
    return count;
}
int colorPixels(const QImage& image, QColor color) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixel(x, y) == color.rgba()) ++count;
    return count;
}
}

// A separate process is essential: the input gate deliberately excludes mt's
// own windows. It receives only test keystrokes and never edits a real document.
class GestureTarget final : public QWidget {
    std::unique_ptr<QWidget> secondary_;
  public:
    int downs = 0, ups = 0, actions = 0, contexts = 0, refreshes = 0, deletes = 0;
    GestureTarget() { setWindowTitle(QStringLiteral("mt gesture test target")); resize(720, 440); }
  protected:
    bool nativeEvent(const QByteArray&, void* raw, qintptr* result) override {
        const auto* msg = static_cast<MSG*>(raw);
        if (msg->message == WM_RBUTTONDOWN || msg->message == WM_RBUTTONDBLCLK) ++downs;
        if (msg->message == WM_RBUTTONUP) ++ups;
        if (msg->message == WM_APP + 71) {
            *result = msg->wParam == 1 ? downs : msg->wParam == 2 ? ups :
                      msg->wParam == 3 ? actions : msg->wParam == 5 ? refreshes :
                      msg->wParam == 6 ? deletes : contexts;
            return true;
        }
        if (msg->message == WM_APP + 72) {
            secondary_ = std::make_unique<QWidget>();
            secondary_->setWindowTitle(QStringLiteral("mt gesture second target"));
            secondary_->setGeometry(180, 180, 480, 320);
            secondary_->show();
            *result = qintptr(secondary_->winId());
            return true;
        }
        if (msg->message == WM_APP + 73) {
            secondary_.reset();
            *result = 1;
            return true;
        }
        return false;
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F5) ++refreshes;
        if (event->key() == Qt::Key_Delete) ++deletes;
        if (event->key() == Qt::Key_F6 && event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
            ++actions;
    }
    void contextMenuEvent(QContextMenuEvent* event) override { ++contexts; event->accept(); }
};

class GestureTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void clickAndMovementAreDistinct() {
        GestureCapture capture;
        QVERIFY(capture.begin({100, 100}, 10));
        QVERIFY(!capture.begin({200, 200}, 11));
        capture.move({102, 101}, 20);
        QCOMPARE(capture.release({101, 102}, 30), GestureCapture::Result::Click);
        QVERIFY(!capture.held());
        QCOMPARE(capture.release({500, 500}, 40), GestureCapture::Result::Pending);
        capture.begin({100, 100}, 50);
        capture.move({130, 100}, 60);
        QCOMPARE(capture.release({180, 100}, 70), GestureCapture::Result::Stroke);
        QCOMPARE(capture.points().front(), QPoint(100, 100));
    }
    void timeoutAndCancellationOwnTheRelease() {
        GestureCapture capture;
        capture.begin({0, 0}, 0);
        capture.expire(1500, 1000);
        QVERIFY(capture.held());
        QCOMPARE(capture.release({0, 0}, 1600), GestureCapture::Result::Click);
        capture.begin({0, 0}, 2000);
        capture.move({50, 0}, 2100);
        capture.expire(3100, 1000);
        QVERIFY(capture.cancelled());
        QCOMPARE(capture.release({50, 50}, 3200), GestureCapture::Result::Cancelled);
        capture.begin({0, 0}, 4000);
        capture.cancel(); // Escape: no click or action
        QCOMPARE(capture.release({0, 0}, 4100), GestureCapture::Result::Cancelled);
        capture.begin({0, 0}, 5000);
        capture.cancel(true); // disabled before drawing: ordinary click still paired
        QCOMPARE(capture.release({0, 0}, 5100), GestureCapture::Result::Click);
        capture.begin({0, 0}, 6000);
        capture.expire(50000, 0);
        QVERIFY(!capture.cancelled());
        capture.expire(50000, 1000);
        capture.cancel(); // explicit cancellation overrides timeout click restoration
        QCOMPARE(capture.release({0, 0}, 50010), GestureCapture::Result::Cancelled);
    }
    void sampleOverflowNeverExecutesATruncatedStroke() {
        GestureCapture capture;
        capture.begin({}, 0);
        for (int i = 1; i < 4200; ++i) capture.move({i * 3, 0}, quint64(i));
        QVERIFY(capture.points().size() <= 4096);
        QCOMPARE(capture.release({13000, 0}, 4300), GestureCapture::Result::Cancelled);
    }
    void directionOrderAndDiagonalArePreserved() {
        const QList<QPair<QPoint, QString>> cases{{{60, 0}, QStringLiteral("6")},
            {{-60, 0}, QStringLiteral("4")}, {{0, 60}, QStringLiteral("2")},
            {{0, -60}, QStringLiteral("8")}, {{60, 60}, QStringLiteral("3")},
            {{-60, 60}, QStringLiteral("1")}, {{-60, -60}, QStringLiteral("7")},
            {{60, -60}, QStringLiteral("9")}};
        for (const auto& [point, expected] : cases) {
            GestureRecognizer recognizer;
            recognizer.begin({0, 0});
            for (int i = 1; i <= 10; ++i) recognizer.append(QPointF(point) * (i / 10.0));
            QCOMPARE(recognizer.directions(), expected);
        }
        GestureRecognizer v;
        v.begin({0, 0}); v.append({60, 60}); v.append({120, 0});
        QCOMPARE(v.directions(), QStringLiteral("39"));
        GestureRecognizer reverse;
        reverse.begin({0, 0}); reverse.append({0, 60}); reverse.append({0, 0});
        QCOMPARE(reverse.directions(), QStringLiteral("28"));
    }
    void rulesRejectMalformedAndDuplicateInput() {
        QCOMPARE(*normalizeGesture(QStringLiteral("266")), QStringLiteral("26"));
        QVERIFY(!normalizeGesture(QStringLiteral("25")));
        QVERIFY(!normalizeGesture(QStringLiteral("2468246824682468")));
        QVERIFY(!GestureRules::parse({{QStringLiteral("26"), QStringLiteral("cmd:none")},
                                     {QStringLiteral("266"), QStringLiteral("cmd:copy")}}, {}));
        QVERIFY(!GestureAction::parse(QStringLiteral("cmd:arbitrary")));
        QVERIFY(!GestureAction::parse(QStringLiteral("key:Ctrl+K, Ctrl+C")));
        QVERIFY(!GestureAction::parse(QStringLiteral("key:")));
        QVERIFY(!GestureAction::parse(QStringLiteral("run:")));
        QVERIFY(GestureAction::parse(QStringLiteral("key:Ctrl+Shift+F6")));
        QCOMPARE(GestureAction::parse(QStringLiteral("cmd:close"))->value, QStringLiteral("close_window"));
        QCOMPARE(GestureAction::parse(QStringLiteral("cmd:directory"))->value, QStringLiteral("open_exe_dir"));
        for (const auto& command : gestureCommands())
            QVERIFY(GestureAction::parse(QStringLiteral("cmd:") + command));
        QVERIFY(!GestureRules::parse({}, {}, {QStringLiteral("C:\\game.exe")}));
        QVERIFY(!GestureRules::parse({}, {}, {}, {QStringLiteral("*.exe")}));
    }
    void perAppRulesAndGates() {
        auto rules = GestureRules::parse({{QStringLiteral("6"), QStringLiteral("key:Alt+Right")},
                                         {QStringLiteral("26"), QStringLiteral("cmd:close_window")}},
            {{QStringLiteral("Chrome.EXE"), QJsonObject{{QStringLiteral("26"), QStringLiteral("cmd:none")}}}},
            {QStringLiteral("blocked.exe")}, {QStringLiteral("chrome.exe"), QStringLiteral("blocked.exe")});
        QVERIFY(rules);
        QVERIFY(rules->permits(QStringLiteral("CHROME.EXE")));
        QVERIFY(!rules->permits(QStringLiteral("blocked.exe")));
        QVERIFY(!rules->permits(QStringLiteral("other.exe")));
        QCOMPARE(rules->forProcess(QStringLiteral("CHROME.EXE")).value(QStringLiteral("26")).value,
                 QStringLiteral("none"));
        QCOMPARE(rules->forProcess(QStringLiteral("chrome.exe")).size(), 2);
    }
    void settingsRoundTripAndValidation() {
        auto settings = Settings::defaults();
        QVERIFY(settings.validate());
        settings.gesturesEnabled = true;
        settings.gestureTrigger = QStringLiteral("x2");
        settings.gestureTimeout = 0;
        settings.gestureAllowlist = {QStringLiteral("chrome.exe")};
        const auto loaded = Settings::decode(settings.encode());
        QVERIFY(loaded);
        QVERIFY(loaded->gesturesEnabled);
        QCOMPARE(loaded->gestureTrigger, QStringLiteral("x2"));
        QCOMPARE(loaded->gestureApplicationActions, settings.gestureApplicationActions);
        QVERIFY(!Settings::decode(R"({"schema":1,"excludedProcesses":[23]})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"gestureApplicationActions":[]})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"gestureActions":{"6":"key:???"}})"));
        QVERIFY(!Settings::decode(R"({"schema":1,"gestureTimeout":1})"));
    }
    void editorCommitsValidRulesAndCancelsDrafts() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        auto settings = Settings::defaults();
        GestureSettings editor(theme, settings);
        auto* table = editor.findChild<QTableWidget*>(QStringLiteral("gestureRules"));
        QCOMPARE(table->rowCount(), 13);
        editor.findChild<QPushButton*>(QStringLiteral("editGesture"))->click();
        QPointer<QDialog> modal = editor.findChild<QDialog*>(QStringLiteral("gestureEditor"));
        QVERIFY(modal);
        modal->findChild<QLineEdit*>(QStringLiteral("gestureDirections"))->setText(QStringLiteral("28"));
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(modal.isNull());
        QVERIFY(editor.write(settings));
        QVERIFY(settings.gestureActions.contains(QStringLiteral("28")));
        QVERIFY(!settings.gestureActions.contains(QStringLiteral("1")));
        editor.findChild<QPushButton*>(QStringLiteral("addGesture"))->click();
        modal = editor.findChild<QDialog*>(QStringLiteral("gestureEditor"));
        modal->findChild<QLineEdit*>(QStringLiteral("gestureDirections"))->setText(QStringLiteral("28"));
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(modal && modal->isVisible()); // duplicate must not enter the draft
        QCOMPARE(table->rowCount(), 13);
        modal->findChild<QLineEdit*>(QStringLiteral("gestureDirections"))->setText(QStringLiteral("248"));
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QTRY_VERIFY(modal.isNull());
        QVERIFY(editor.write(settings));
        QVERIFY(!settings.gestureActions.contains(QStringLiteral("248")));
        editor.findChild<QPushButton*>(QStringLiteral("removeGesture"))->click();
        QPointer<QMessageBox> confirm = editor.findChild<QMessageBox*>(QStringLiteral("deleteGestureConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(confirm.isNull());
        QVERIFY(editor.write(settings));
        QVERIFY(!settings.gestureActions.contains(QStringLiteral("28")));
        QCOMPARE(table->rowCount(), 12);
    }
    void overlayDoesNotTakeInputAndBothThemesRender() {
        Theme theme;
        for (const auto& mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            theme.apply(mode);
            GestureOverlay overlay(theme);
            QVERIFY(overlay.windowFlags() & Qt::WindowTransparentForInput);
            QVERIFY(overlay.windowFlags() & Qt::WindowDoesNotAcceptFocus);
            QVERIFY(overlay.testAttribute(Qt::WA_ShowWithoutActivating));
            overlay.setGeometry(100, 100, 600, 400);
            overlay.present({QPoint(160, 170), QPoint(300, 310), QPoint(440, 170)}, QStringLiteral("刷新"), true);
            const auto image = overlay.grab().toImage();
            QVERIFY(!image.isNull());
            QVERIFY(colorPixels(image, QColor(Qt::white)) > 20);
            QCOMPARE(theme.color(QStringLiteral("gesture-trail")), QColor(0, 160, 255));
            QCOMPARE(theme.color(QStringLiteral("gesture-text")), QColor(Qt::white));
            // The name is at the bottom of this monitor; there is no cursor bubble.
            QVERIFY(visiblePixels(image, {180, 248, 240, 45}) > 100);
            QCOMPARE(visiblePixels(image, {360, 90, 200, 100}), 0);
            const auto folder = qEnvironmentVariable("MT_GESTURE_EVIDENCE");
            if (!folder.isEmpty()) {
                QDir().mkpath(folder);
                QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("trail-%1.png").arg(mode))));
                GestureSettings editor(theme, Settings::defaults());
                editor.resize(560, editor.sizeHint().height());
                QVERIFY(editor.grab().save(QDir(folder).filePath(QStringLiteral("settings-%1.png").arg(mode))));
                overlay.present({QPoint(160, 170), QPoint(440, 170)}, QStringLiteral("手势无动作"), true, false);
                QVERIFY(overlay.grab().save(QDir(folder).filePath(QStringLiteral("trail-unmatched-%1.png").arg(mode))));
            }
            overlay.dismiss();
        }
    }
    void dismissedOverlayCannotExposeThePreviousStroke() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        GestureOverlay overlay(theme);
        overlay.setGeometry(100, 100, 600, 400);
        overlay.present({QPoint(140, 150), QPoint(360, 150)}, QStringLiteral("复制"), true);
        QVERIFY(QTest::qWaitForWindowExposed(&overlay));
        QVERIFY(visiblePixels(backingPixels(overlay), {0, 0, 600, 400}) > 100);
        overlay.dismiss();
        QVERIFY(!overlay.isVisible());
        // Read the real raster backing store without grab()/render(), which
        // would repaint and could conceal a stale native surface regression.
        const auto cleared = backingPixels(overlay);
        QVERIFY(!cleared.isNull());
        QCOMPARE(visiblePixels(cleared, {0, 0, 600, 400}), 0);
        overlay.present({QPoint(140, 260), QPoint(360, 260)}, {}, true);
        const auto next = backingPixels(overlay);
        QVERIFY(!next.isNull());
        QCOMPARE(visiblePixels(next, {0, 35, 600, 30}), 0);
        QCOMPARE(visiblePixels(next, {0, 245, 600, 60}), 0);
        QTRY_VERIFY(visiblePixels(backingPixels(overlay), {0, 145, 600, 30}) > 100);
        QCOMPARE(visiblePixels(backingPixels(overlay), {0, 35, 600, 30}), 0);
        // A stationary right press may not show either a dot or an old name.
        overlay.present({QPoint(140, 260)}, {}, true);
        QVERIFY(!overlay.isVisible());
        QCOMPARE(visiblePixels(backingPixels(overlay), {0, 0, 600, 400}), 0);
    }
    void overlayMatchesLegacyTrailGeometryAndNamePlacement() {
        Theme theme; theme.apply(QStringLiteral("light"));
        GestureOverlay overlay(theme);
        overlay.setGeometry(0, 0, 600, 400);
        overlay.present({QPoint(50, 100), QPoint(200, 100)}, QStringLiteral("复制"), true);
        const auto image = overlay.grab().toImage();
        QVERIFY(visiblePixels(image, {47, 96, 7, 8}) > 20); // 8px start dot
        QVERIFY(visiblePixels(image, {180, 93, 8, 14}) > 40); // filled arrow
        QCOMPARE(visiblePixels(image, {80, 103, 50, 5}), 0); // no 6px outline
        QVERIFY(visiblePixels(image, {220, 248, 160, 45}) > 100);
        overlay.present({QPoint(50, 100), QPoint(200, 100)}, QStringLiteral("手势无动作"), false, false);
        const auto unmatched = overlay.grab().toImage();
        QVERIFY(colorPixels(unmatched, QColor(102, 102, 102)) > 20);
        QVERIFY(colorPixels(unmatched, QColor(Qt::white)) > 20);
        // The name belongs only to the monitor containing the current endpoint.
        overlay.present({QPoint(50, 100), QPoint(700, 100)}, QStringLiteral("复制"), false);
        QCOMPARE(visiblePixels(overlay.grab().toImage(), {0, 0, 600, 400}), 0);
        // Long visible tails are capped; the true start is not moved to the cut.
        overlay.present({QPoint(50, 200), QPoint(-2600, 200), QPoint(-2600, 100), QPoint(500, 100)}, {}, true);
        const auto longTrail = overlay.grab().toImage();
        QVERIFY(visiblePixels(longTrail, {400, 95, 100, 12}) > 100);
        QCOMPARE(visiblePixels(longTrail, {0, 190, 200, 20}), 0);
        overlay.dismiss();
    }
    void overlayKeepsSystemFontAndFractionalAntialiasing() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        GestureOverlay overlay(theme);
        overlay.setGeometry(0, 0, 600, 400);
        QCOMPARE(overlay.font().family(), QApplication::font().family());
        QCOMPARE(overlay.font().pixelSize(), 26);
        QCOMPARE(overlay.font().weight(), QApplication::font().weight());
        qInfo() << "Gesture font:" << overlay.font().family() << "pixels:" << overlay.font().pixelSize()
                << "DPR:" << overlay.devicePixelRatioF();
        QPolygonF points{QPointF(50.25, 60.5), QPointF(241.75, 180.25), QPointF(390.5, 72.75)};
        overlay.present(points, QStringLiteral("最大化/还原"), true);
        const auto initial = overlay.grab().toImage();
        QSet<int> coverage;
        for (int y = 0; y < initial.height(); ++y)
            for (int x = 0; x < initial.width(); ++x) {
                const int alpha = qAlpha(initial.pixel(x, y));
                if (alpha > 0 && alpha < 255) coverage.insert(alpha);
            }
        QVERIFY(coverage.size() > 32); // actual grayscale edge coverage, not a binary mask
        overlay.present(points.translated(0.35, 0.35), QStringLiteral("最大化/还原"), true);
        QVERIFY(initial != overlay.grab().toImage()); // subpixel movement is not rounded away
        const auto folder = qEnvironmentVariable("MT_GESTURE_EVIDENCE");
        if (!folder.isEmpty())
            QVERIFY(initial.save(QDir(folder).filePath(QStringLiteral("antialias-%1.png")
                                                     .arg(qRound(overlay.devicePixelRatioF() * 100)))));
        overlay.dismiss();
    }
    void fadeCannotHideOrResurrectTheNextStroke() {
        Theme theme; theme.apply(QStringLiteral("dark"));
        GestureOverlay overlay(theme);
        overlay.setGeometry(0, 0, 600, 400);
        overlay.present({QPointF(40, 50), QPointF(260, 50)}, QStringLiteral("最小化"), true);
        QVERIFY(QTest::qWaitForWindowExposed(&overlay));
        overlay.finish();
        QTest::qWait(50);
        QVERIFY(overlay.isVisible());
        QTRY_VERIFY(!overlay.isVisible());
        QCOMPARE(visiblePixels(backingPixels(overlay), {0, 0, 600, 400}), 0);
        overlay.present({QPointF(40, 50), QPointF(260, 50)}, QStringLiteral("最小化"), true);
        overlay.finish();
        QTest::qWait(35);
        overlay.dismiss(); // next right-down cancels the fade, even before moving
        overlay.present({QPointF(40, 160), QPointF(260, 160)}, QStringLiteral("最大化"), true);
        QTest::qWait(180);
        QVERIFY(overlay.isVisible());
        const auto next = backingPixels(overlay);
        QCOMPARE(visiblePixels(next, {0, 35, 600, 30}), 0);
        QVERIFY(visiblePixels(next, {0, 145, 600, 30}) > 100);
        overlay.dismiss();
    }
    void commonActionsHaveLegacyNames() {
        QCOMPARE(GestureAction::parse(QStringLiteral("key:F5"))->label(), tx(u"刷新"));
        QCOMPARE(GestureAction::parse(QStringLiteral("key:Alt+Left"))->label(), tx(u"后退"));
        QCOMPARE(GestureAction::parse(QStringLiteral("key:Ctrl+W"))->label(), tx(u"关闭标签页"));
        QCOMPARE(GestureAction::parse(QStringLiteral("cmd:toggle_maximize"))->label(), tx(u"最大化/还原"));
    }
    void desktopClickStrokeCancelDisableAndTargetChange() {
        if (!qEnvironmentVariableIsSet("MT_GESTURE_DESKTOP_TEST"))
            QSKIP("Opt-in Windows input test, isolated target process only");
        const HWND previous = GetForegroundWindow();
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
        const auto handle = target.readLine().trimmed().toULongLong(&ok, 16);
        QVERIFY(ok);
        const HWND window = reinterpret_cast<HWND>(quintptr(handle));
        QVERIFY(IsWindow(window));
        struct CloseTarget { QProcess& process; HWND window;
            ~CloseTarget() { PostMessageW(window, WM_CLOSE, 0, 0); if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(); } }
        } close{target, window};
        const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = foregroundThread != GetCurrentThreadId() &&
            AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        if (attached) AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
        QTRY_COMPARE(GetForegroundWindow(), window);
        RECT bounds{};
        GetClientRect(window, &bounds);
        POINT origin{100, 100};
        ClientToScreen(window, &origin);
        const auto count = [window](int kind) {
            DWORD_PTR value = 0;
            if (!SendMessageTimeoutW(window, WM_APP + 71, WPARAM(kind), 0, SMTO_ABORTIFHUNG, 1000, &value)) return -1;
            return int(value);
        };
        bool rightHeld = false;
        const auto mouse = [&rightHeld](DWORD flags) {
            INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = flags;
            const bool sent = SendInput(1, &event, sizeof(INPUT)) == 1;
            if (sent) rightHeld = flags == MOUSEEVENTF_RIGHTDOWN;
            return sent;
        };
        const auto absoluteMove = [](LONG x, LONG y) {
            INPUT event{};
            event.type = INPUT_MOUSE;
            event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                               MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
            event.mi.dx = LONG(((qint64(x) - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 2 + 1) * 65536 /
                               (GetSystemMetrics(SM_CXVIRTUALSCREEN) * 2));
            event.mi.dy = LONG(((qint64(y) - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 2 + 1) * 65536 /
                               (GetSystemMetrics(SM_CYVIRTUALSCREEN) * 2));
            // Use the same input stream as the button events, with explicit
            // ordering and zero extra-info instead of SetCursorPos side effects.
            return event;
        };
        const auto movePointer = [&](LONG x, LONG y) {
            auto event = absoluteMove(x, y);
            return SendInput(1, &event, sizeof(INPUT)) == 1;
        };
        const auto overlaysVisible = [] {
            for (const auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("gestureOverlay") && widget->isVisible()) return true;
            return false;
        };
        auto settings = Settings::defaults();
        settings.gesturesEnabled = true;
        settings.disableGesturesFullscreen = false;
        settings.gestureActions = {{QStringLiteral("6"), QStringLiteral("key:Ctrl+Shift+F6")}};
        settings.gestureApplicationActions = {};
        settings.gestureAllowlist = {QFileInfo(QCoreApplication::applicationFilePath()).fileName()};
        QTemporaryDir data;
        Theme theme; theme.apply(QStringLiteral("dark"));
        bool blocked = false;
        GestureController controller(theme, data.path(), false, [&] { return blocked; });
        struct ReleaseButton {
            bool& held;
            ~ReleaseButton() {
                if (held) { INPUT e{}; e.type = INPUT_MOUSE; e.mi.dwFlags = MOUSEEVENTF_RIGHTUP; SendInput(1, &e, sizeof(INPUT)); }
            }
        } releaseButton{rightHeld};
        QSignalSpy failures(&controller, &GestureController::failed);
        QSignalSpy matches(&controller, &GestureController::recognized);
        QVERIFY(controller.configure(settings));
        QTest::qWait(250);
        QVERIFY(movePointer(origin.x, origin.y));
        QTest::qWait(40);
        QCOMPARE(GetAncestor(WindowFromPoint(origin), GA_ROOTOWNER), window);
        QCOMPARE(GetForegroundWindow(), window);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
        QTest::qWait(30);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QCOMPARE(GetForegroundWindow(), window);
        POINT clickCursor{}; QVERIFY(GetCursorPos(&clickCursor));
        QCOMPARE(QPoint(clickCursor.x, clickCursor.y), QPoint(origin.x, origin.y));
        QTRY_COMPARE(count(1), 1);
        QTRY_COMPARE(count(2), 1);
        QTRY_COMPARE(count(4), 1);
        QCOMPARE(matches.size(), 0);
        // Horizontal stroke produces one chord and no context click.
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
        for (int i = 1; i <= 10; ++i) { QVERIFY(movePointer(origin.x + i * 12, origin.y)); QTest::qWait(12); }
        QVERIFY(overlaysVisible());
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QTRY_COMPARE(count(3), 1);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(count(1), 1); QCOMPARE(count(2), 1);
        // Starting the next gesture without moving cannot flash the last one.
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
        QTest::qWait(25); // cancels the old fade before its 150ms deadline
        for (int i = 0; i < 12; ++i) {
            QTest::qWait(8);
            QVERIFY(!overlaysVisible());
        }
        QVERIFY(movePointer(origin.x, origin.y + 80)); // unmapped down stroke
        QTRY_VERIFY(overlaysVisible());
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QTRY_VERIFY(!overlaysVisible());
        QCOMPARE(count(3), 1); QCOMPARE(count(2), 1);
        // Idle timeout and Escape both cancel drawing without a context click.
        settings.gestureTimeout = 200;
        QVERIFY(controller.configure(settings)); QTest::qWait(150);
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
        QVERIFY(movePointer(origin.x + 80, origin.y)); QTest::qWait(300);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP)); QTest::qWait(80);
        QCOMPARE(count(3), 1); QCOMPARE(count(2), 1);
        settings.gestureTimeout = 1000;
        QVERIFY(controller.configure(settings)); QTest::qWait(150);
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN)); QVERIFY(movePointer(origin.x + 80, origin.y));
        INPUT escape[2]{};
        escape[0].type = escape[1].type = INPUT_KEYBOARD;
        escape[0].ki.wVk = escape[1].ki.wVk = VK_ESCAPE;
        escape[1].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2, escape, sizeof(INPUT)), 2u);
        QTest::qWait(30);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP)); QTest::qWait(80);
        QCOMPARE(count(3), 1); QCOMPARE(count(2), 1);
        // An explicit host gate cancels the stroke, but still consumes its Up.
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
        QVERIFY(movePointer(origin.x + 80, origin.y)); QTest::qWait(30);
        blocked = true; controller.blockNow();
        blocked = false; QTest::qWait(60); // reopening the gate cannot revive the old stroke
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP)); QTest::qWait(80);
        QCOMPARE(count(3), 1); QCOMPARE(count(2), 1);
        blocked = false; QTest::qWait(150);
        // Disabling while a stationary button is held returns one paired click.
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN)); QTest::qWait(30);
        settings.gesturesEnabled = false;
        QVERIFY(controller.configure(settings));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QTRY_COMPARE(count(1), 2); QTRY_COMPARE(count(2), 2);
        QCOMPARE(count(3), 1);
        settings.gesturesEnabled = true;
        QVERIFY(controller.configure(settings)); QTest::qWait(200);
        // Changing configuration before the queued release is dispatched must
        // discard the old action even when gestures stay enabled.
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN)); QVERIFY(movePointer(origin.x + 80, origin.y));
        QTest::qWait(30);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QVERIFY(controller.configure(settings)); QTest::qWait(180);
        QCOMPARE(count(3), 1);
        // A new foreground window invalidates the original target.
        QVERIFY(movePointer(origin.x, origin.y));
        QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN)); QVERIFY(movePointer(origin.x + 80, origin.y)); QTest::qWait(30);
        QWidget other; other.resize(200, 100); other.show(); other.activateWindow();
        SetForegroundWindow(reinterpret_cast<HWND>(other.winId())); QTest::qWait(60);
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP)); QTest::qWait(100);
        QCOMPARE(count(3), 1); QCOMPARE(matches.size(), 1);
        other.hide();
        // A native WorkerW fixture exercises desktop routing without minimizing
        // or sending gestures to any of the user's real applications.
        QProcess desktop;
        desktop.setProgram(QCoreApplication::applicationFilePath());
        desktop.setArguments({QStringLiteral("--gesture-desktop")});
        desktop.start();
        QVERIFY(desktop.waitForStarted());
        QVERIFY(desktop.waitForReadyRead(5000));
        const auto desktopHandle = desktop.readLine().trimmed().toULongLong(&ok, 16);
        QVERIFY(ok);
        const auto desktopWindow = reinterpret_cast<HWND>(quintptr(desktopHandle));
        QVERIFY(IsWindow(desktopWindow));
        CloseTarget closeDesktop{desktop, desktopWindow};
        const auto focus = [](HWND selected) {
            const auto foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            const bool attached = foregroundThread && foregroundThread != GetCurrentThreadId() &&
                AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
            BringWindowToTop(selected);
            SetForegroundWindow(selected);
            if (attached) AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
            QTest::qWait(40);
            return GetForegroundWindow() == selected;
        };
        const auto draw = [&](HWND selected, QPoint delta, bool activate = true) {
            if (activate && !focus(selected)) return false;
            POINT point{180, 160};
            if (!ClientToScreen(selected, &point) || !movePointer(point.x, point.y)) return false;
            QTest::qWait(35); // settle the off-hook identity/permission cache
            if (GetAncestor(WindowFromPoint(point), GA_ROOTOWNER) != selected) return false;
            if (!mouse(MOUSEEVENTF_RIGHTDOWN)) return false;
            for (int i = 1; i <= 8; ++i) {
                if (!movePointer(point.x + delta.x() * i / 8, point.y + delta.y() * i / 8)) return false;
                QTest::qWait(12);
            }
            if (!mouse(MOUSEEVENTF_RIGHTUP)) return false;
            QTest::qWait(60);
            return true;
        };
        settings.disableGesturesFullscreen = true; // WorkerW is never a fullscreen app
        settings.gestureActions = {{QStringLiteral("1"), QStringLiteral("cmd:minimize")},
                                   {QStringLiteral("9"), QStringLiteral("cmd:toggle_maximize")},
                                   {QStringLiteral("8"), QStringLiteral("cmd:restore")}};
        QVERIFY(controller.configure(settings));
        DWORD_PTR secondHandle = 0;
        QVERIFY(SendMessageTimeoutW(window, WM_APP + 72, 0, 0, SMTO_ABORTIFHUNG, 2000, &secondHandle));
        const auto secondWindow = reinterpret_cast<HWND>(secondHandle);
        QVERIFY(IsWindow(secondWindow));
        QVERIFY(draw(window, {-72, 72}));
        QTRY_VERIFY(IsIconic(window));
        QVERIFY(draw(secondWindow, {-72, 72}));
        QTRY_VERIFY(IsIconic(secondWindow));
        QVERIFY(draw(desktopWindow, {72, -72}));
        QTRY_VERIFY(!IsIconic(secondWindow) && IsZoomed(secondWindow));
        QVERIFY(IsIconic(window)); // only the LAST minimized target is recalled
        const auto afterRecall = matches.size();
        QVERIFY(draw(desktopWindow, {72, -72}));
        QCOMPARE(matches.size(), afterRecall); // consumed record: no unrelated action
        // Manually restoring a saved target invalidates the recall.
        QVERIFY(draw(secondWindow, {-72, 72}));
        QTRY_VERIFY(IsIconic(secondWindow));
        ShowWindowAsync(secondWindow, SW_RESTORE);
        QTRY_VERIFY(!IsIconic(secondWindow));
        ShowWindowAsync(secondWindow, SW_RESTORE); // leave the formerly zoomed window normal
        QTRY_VERIFY(!IsZoomed(secondWindow));
        const auto afterManualRestore = matches.size();
        QVERIFY(draw(desktopWindow, {72, -72}));
        QCOMPARE(matches.size(), afterManualRestore);
        QVERIFY(!IsZoomed(secondWindow));
        // Pointer target may be inactive: minimize it without operating on the
        // separate foreground window. Its frame is positioned clear of WorkerW.
        MONITORINFO monitor{sizeof(monitor)};
        QVERIFY(GetMonitorInfoW(MonitorFromWindow(desktopWindow, MONITOR_DEFAULTTONEAREST), &monitor));
        const int halfWidth = (monitor.rcWork.right - monitor.rcWork.left) / 2;
        MoveWindow(desktopWindow, monitor.rcWork.left + 10, monitor.rcWork.top + 10,
                   halfWidth - 20, 400, TRUE);
        MoveWindow(secondWindow, monitor.rcWork.left + halfWidth + 10, monitor.rcWork.top + 10,
                   halfWidth - 20, 400, TRUE);
        QVERIFY(focus(desktopWindow));
        QVERIFY(draw(secondWindow, {-72, 72}, false));
        QTRY_VERIFY(IsIconic(secondWindow));
        QVERIFY(draw(desktopWindow, {0, -80}));
        QTRY_VERIFY(!IsIconic(secondWindow) && !IsZoomed(secondWindow));
        // A closed saved target is never replaced with another HWND.
        QVERIFY(draw(secondWindow, {-72, 72}));
        QTRY_VERIFY(IsIconic(secondWindow));
        DWORD_PTR closed = 0;
        QVERIFY(SendMessageTimeoutW(window, WM_APP + 73, 0, 0, SMTO_ABORTIFHUNG, 2000, &closed));
        QCOMPARE(closed, DWORD_PTR(1));
        QTRY_VERIFY(!IsWindow(secondWindow));
        const auto afterClose = matches.size();
        QVERIFY(draw(desktopWindow, {72, -72}));
        QCOMPARE(matches.size(), afterClose);
        // Retaining an old completion for execution must not flash that path
        // after a new stationary press has already started in the same burst.
        ShowWindowAsync(window, SW_RESTORE);
        QTRY_VERIFY(!IsIconic(window));
        QVERIFY(focus(window));
        settings.gestureActions = {{QStringLiteral("6"), QStringLiteral("key:Ctrl+Shift+F6")}};
        QVERIFY(controller.configure(settings));
        POINT burstStart{140, 140};
        QVERIFY(ClientToScreen(window, &burstStart));
        QVERIFY(GetAncestor(WindowFromPoint(burstStart), GA_ROOTOWNER) == window);
        const int beforeActions = count(3);
        const auto beforeMatches = matches.size();
        INPUT press{}; press.type = INPUT_MOUSE; press.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        auto release = press; release.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        std::array<INPUT, 5> burst{absoluteMove(burstStart.x, burstStart.y), press,
            absoluteMove(burstStart.x + 80, burstStart.y), release, press};
        rightHeld = true;
        QCOMPARE(SendInput(UINT(burst.size()), burst.data(), sizeof(INPUT)), UINT(burst.size()));
        QTest::qWait(60);
        QTRY_COMPARE(count(3), beforeActions + 1);
        QCOMPARE(matches.size(), beforeMatches + 1);
        QVERIFY(!overlaysVisible());
        QVERIFY(movePointer(burstStart.x + 160, burstStart.y));
        QTRY_VERIFY(overlaysVisible());
        QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
        QTRY_COMPARE(count(3), beforeActions + 2);
        QCOMPARE(matches.size(), beforeMatches + 2);
        // Exercise the actual default V -> F5 binding through native samples,
        // controller matching and key injection into this isolated process.
        settings.gestureActions = Settings::defaults().gestureActions;
        QVERIFY(controller.configure(settings));
        const int beforeVDowns = count(1), beforeVUps = count(2), beforeVContexts = count(4);
        const auto beforeVMatches = matches.size();
        const auto drawV = [&](double angle, double noise, bool partial = false) {
            POINT start{100, 80};
            if (!ClientToScreen(window, &start) || !movePointer(start.x, start.y)) return false;
            if (!mouse(MOUSEEVENTF_RIGHTDOWN)) return false;
            const double halfWidth = 160.0 / std::tan(angle * std::numbers::pi / 180.0);
            for (int arm = 0; arm < 2; ++arm)
                for (int i = 1; i <= (partial && arm == 1 ? 5 : 45); ++i) {
                    const double t = i / 45.0;
                    const QPoint planned(start.x + qRound(halfWidth * (arm + t) + noise * std::sin(i * 2.3)),
                                         start.y + qRound(160.0 * (arm == 0 ? t : 1.0 - t)));
                    if (!movePointer(planned.x(), planned.y())) return false;
                    QTest::qWait(3);
                    POINT actual{};
                    if (!GetCursorPos(&actual) || (QPoint(actual.x, actual.y) - planned).manhattanLength() > 1) {
                        qWarning() << "External pointer movement interrupted native input test; planned" << planned
                                   << "actual" << QPoint(actual.x, actual.y);
                        return false;
                    }
                }
            return mouse(MOUSEEVENTF_RIGHTUP);
        };
        int refreshes = count(5);
        for (int angle : {30,45,60,65,75,80})
            for (int noise : {0,2}) {
                QVERIFY(drawV(angle, noise));
                ++refreshes;
                QTRY_COMPARE(count(5), refreshes);
                QCOMPARE(matches.last().first().toString(), QStringLiteral("39"));
            }
        QCOMPARE(matches.size(), beforeVMatches + 12);
        QCOMPARE(count(1), beforeVDowns); QCOMPARE(count(2), beforeVUps); QCOMPARE(count(4), beforeVContexts);
        QVERIFY(drawV(45, 0, true));
        QTest::qWait(200);
        QCOMPARE(count(5), refreshes); QCOMPARE(count(6), 0);
        QCOMPARE(matches.size(), beforeVMatches + 12);
        // An explicitly disabled V still participates in recognition; it cannot
        // fall through to Delete or another global action.
        settings.gestureApplicationActions = {{QFileInfo(QCoreApplication::applicationFilePath()).fileName(),
            QJsonObject{{QStringLiteral("39"), QStringLiteral("cmd:none")}}}};
        QVERIFY(controller.configure(settings));
        QVERIFY(drawV(45, 2));
        QTest::qWait(200);
        QCOMPARE(count(5), refreshes); QCOMPARE(count(6), 0);
        QCOMPARE(matches.size(), beforeVMatches + 12);
        const int clickDowns = count(1), clickUps = count(2), clickMenus = count(4);
        int clicks = 0;
        for (int hold : {0,30,80})
            for (int repetition = 0; repetition < 8; ++repetition) {
                QCOMPARE(GetForegroundWindow(), window);
                QVERIFY(mouse(MOUSEEVENTF_RIGHTDOWN));
                if (hold) QTest::qWait(hold);
                QVERIFY(mouse(MOUSEEVENTF_RIGHTUP));
                QCOMPARE(GetForegroundWindow(), window);
                ++clicks;
                QTRY_COMPARE(count(1), clickDowns + clicks);
                QTRY_COMPARE(count(2), clickUps + clicks);
                QTRY_COMPARE(count(4), clickMenus + clicks);
            }
        qInfo() << "Default V -> F5 exactly once: 12; unfinished/disabled V blocked; held ordinary clicks:" << clicks;
        QVERIFY2(failures.isEmpty(), failures.isEmpty() ? "" : qPrintable(failures.first().first().toString()));
    }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(QStringLiteral("Fusion"));
    app.setFont(DesktopServices::systemUiFont());
    if (app.arguments().contains(QStringLiteral("--gesture-desktop"))) {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WorkerW";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
            if (message == WM_DESTROY) { QCoreApplication::quit(); return 0; }
            return DefWindowProcW(window, message, w, l);
        };
        if (!RegisterClassW(&type)) return 4;
        const auto desktop = CreateWindowExW(0, type.lpszClassName, L"mt isolated gesture desktop",
            WS_OVERLAPPEDWINDOW, 80, 80, 950, 700, nullptr, nullptr, type.hInstance, nullptr);
        if (!desktop) return 5;
        ShowWindow(desktop, SW_SHOWNORMAL);
        std::cout << std::hex << quintptr(desktop) << std::endl;
        return app.exec();
    }
    if (app.arguments().contains(QStringLiteral("--gesture-target"))) {
        GestureTarget target;
        target.move(120, 120); target.show(); target.activateWindow();
        std::cout << std::hex << quintptr(target.winId()) << std::endl;
        app.setQuitOnLastWindowClosed(true);
        return app.exec();
    }
    GestureTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GestureTests.moc"
