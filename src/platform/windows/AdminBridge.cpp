#include <windows.h>

#include "AdminBridge.h"
#include "GestureActions.h"
#include "InputTag.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <shellapi.h>
#include <wil/resource.h>

namespace mt {
namespace {
constexpr qsizetype maxMessage = 8192;
quint64 started(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    return GetProcessTimes(process, &creation, &exit, &kernel, &user)
               ? (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime
               : 0;
}
QString executable(HANDLE process) {
    wchar_t path[32768];
    DWORD length = DWORD(std::size(path));
    return QueryFullProcessImageNameW(process, 0, path, &length)
               ? QFileInfo(QString::fromWCharArray(path, int(length))).canonicalFilePath()
               : QString{};
}
bool elevated(HANDLE process) {
    wil::unique_handle token;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return OpenProcessToken(process, TOKEN_QUERY, token.put()) &&
           GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation),
                               &bytes) &&
           elevation.TokenIsElevated;
}
bool sameSession(DWORD other) {
    DWORD ours = 0, theirs = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &ours) &&
           ProcessIdToSessionId(other, &theirs) && ours == theirs;
}
QJsonObject targetJson(WindowTarget target) {
    return {{QStringLiteral("window"), QString::number(target.handle)},
            {QStringLiteral("pid"), QString::number(target.processId)},
            {QStringLiteral("started"), QString::number(target.processStarted)},
            {QStringLiteral("focus"), QString::number(target.focusedControl)}};
}
WindowTarget targetFrom(const QJsonObject& o) {
    return {quintptr(o.value(QStringLiteral("window")).toString().toULongLong()),
            o.value(QStringLiteral("pid")).toString().toUInt(),
            o.value(QStringLiteral("started")).toString().toULongLong(),
            quintptr(o.value(QStringLiteral("focus")).toString().toULongLong())};
}
void send(QLocalSocket* socket, const QJsonObject& object) {
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
}
bool receive(QLocalSocket* socket, QByteArray& buffer,
             const std::function<bool(const QJsonObject&)>& consume) {
    buffer += socket->readAll();
    if (buffer.size() > maxMessage * 2)
        return false;
    while (true) {
        const auto newline = buffer.indexOf('\n');
        if (newline < 0)
            return buffer.size() <= maxMessage;
        if (newline > maxMessage)
            return false;
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(buffer.first(newline), &error);
        buffer.remove(0, newline + 1);
        if (error.error != QJsonParseError::NoError || !json.isObject() || !consume(json.object()))
            return false;
    }
}
std::expected<void, QString> replayClick(const QJsonObject& request, WindowTarget target) {
    const auto foreground = request.value(QStringLiteral("foreground")).toString().toULongLong();
    POINT cursor{};
    if (!GetCursorPos(&cursor))
        return std::unexpected(QStringLiteral("无法确认鼠标位置"));
    const auto hit = WindowFromPoint(cursor);
    const auto root = GetAncestor(hit, GA_ROOTOWNER);
    const QPoint expected(request.value(QStringLiteral("x")).toInt(INT_MAX),
                          request.value(QStringLiteral("y")).toInt(INT_MAX));
    if (quintptr(GetForegroundWindow()) != foreground ||
        quintptr(root ? root : hit) != target.handle ||
        qAbs(qint64(cursor.x) - expected.x()) + qAbs(qint64(cursor.y) - expected.y()) > 8 ||
        !DesktopServices::modifiersReleased())
        return std::unexpected(QStringLiteral("目标或鼠标位置已改变，已取消点击回放"));
    const auto button = request.value(QStringLiteral("button")).toString();
    DWORD down = 0, up = 0, data = 0;
    if (button == QStringLiteral("right")) {
        down = MOUSEEVENTF_RIGHTDOWN;
        up = MOUSEEVENTF_RIGHTUP;
    } else if (button == QStringLiteral("middle")) {
        down = MOUSEEVENTF_MIDDLEDOWN;
        up = MOUSEEVENTF_MIDDLEUP;
    } else if (button == QStringLiteral("x1") || button == QStringLiteral("x2")) {
        down = MOUSEEVENTF_XDOWN;
        up = MOUSEEVENTF_XUP;
        data = button == QStringLiteral("x1") ? XBUTTON1 : XBUTTON2;
    } else
        return std::unexpected(QStringLiteral("鼠标按键无效"));
    INPUT inputs[2]{};
    for (auto& input : inputs) {
        input.type = INPUT_MOUSE;
        input.mi.mouseData = data;
        input.mi.dwExtraInfo = ownInputTag;
    }
    inputs[0].mi.dwFlags = down;
    inputs[1].mi.dwFlags = up;
    const auto count = SendInput(2, inputs, sizeof(INPUT));
    if (count == 1)
        SendInput(1, &inputs[1], sizeof(INPUT));
    if (count != 2)
        return std::unexpected(QStringLiteral("系统拒绝恢复鼠标点击"));
    return {};
}
} // namespace
quint64 currentProcessStarted() {
    return started(GetCurrentProcess());
}
struct AdminBridge::Impl {
    AdminBridge* q;
    QLocalServer server;
    QPointer<QLocalSocket> socket;
    QTimer deadline, heartbeat, clickDeadline;
    wil::unique_handle child;
    DWORD childPid = 0;
    bool ready = false, preview = false, stopping = false;
    QString status = QStringLiteral("未启用管理员支持"), requestId;
    QSet<QString> clicks;
    QByteArray buffer;
    Completion starting, completion;
    QPointer<QObject> context;
    explicit Impl(AdminBridge* owner) : q(owner) {
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.setMaxPendingConnections(2);
        deadline.setSingleShot(true);
        clickDeadline.setSingleShot(true);
        QObject::connect(&clickDeadline, &QTimer::timeout, q,
                         [this] { fail(QStringLiteral("管理员点击回放超时，已停止辅助连接")); });
        QObject::connect(&deadline, &QTimer::timeout, q,
                         [this] { fail(QStringLiteral("管理员辅助进程响应超时，请重新启用")); });
        heartbeat.setInterval(500);
        QObject::connect(&heartbeat, &QTimer::timeout, q, [this] {
            if (socket && ready)
                send(socket, {{QStringLiteral("op"), QStringLiteral("ping")}});
        });
        QObject::connect(&server, &QLocalServer::newConnection, q, [this] {
            while (server.hasPendingConnections()) {
                auto* candidate = server.nextPendingConnection();
                ULONG peer = 0;
                if (socket ||
                    !GetNamedPipeClientProcessId(
                        reinterpret_cast<HANDLE>(candidate->socketDescriptor()), &peer) ||
                    peer != childPid || !child ||
                    WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT ||
                    (!preview && !elevated(child.get()))) {
                    candidate->abort();
                    candidate->deleteLater();
                    continue;
                }
                socket = candidate;
                candidate->setReadBufferSize(maxMessage * 2 + 1);
                QObject::connect(candidate, &QLocalSocket::disconnected, q, [this] {
                    if (!stopping)
                        fail(QStringLiteral("管理员辅助进程已断开，可关闭开关后重新启用"));
                });
                QObject::connect(candidate, &QLocalSocket::readyRead, q, [this] {
                    if (!socket)
                        return;
                    const bool valid = receive(socket, buffer, [this](const QJsonObject& response) {
                        if (!ready) {
                            if (response.value(QStringLiteral("hello")).toInt() != 1 ||
                                response.value(QStringLiteral("preview")).toBool() != preview)
                                return false;
                            ready = true;
                            deadline.stop();
                            heartbeat.start();
                            status = preview ? QStringLiteral("隔离验证连接已建立")
                                             : QStringLiteral("管理员支持已连接");
                            Q_EMIT q->changed();
                            auto done = std::move(starting);
                            if (done)
                                done({});
                            return true;
                        }
                        if (response.value(QStringLiteral("pong")).toBool())
                            return true;
                        if (response.value(QStringLiteral("click")).toBool()) {
                            if (!clicks.remove(response.value(QStringLiteral("id")).toString()))
                                return false;
                            if (clicks.isEmpty())
                                clickDeadline.stop();
                            if (!response.value(QStringLiteral("ok")).toBool())
                                Q_EMIT q->failed(
                                    response.value(QStringLiteral("error")).toString());
                            return true;
                        }
                        if (!completion ||
                            response.value(QStringLiteral("id")).toString() != requestId)
                            return false;
                        deadline.stop();
                        auto done = std::move(completion);
                        const auto receiver = context;
                        context = nullptr;
                        requestId.clear();
                        const bool ok = response.value(QStringLiteral("ok")).toBool();
                        if (receiver)
                            done(ok ? std::expected<void, QString>{}
                                    : std::unexpected(
                                          response.value(QStringLiteral("error"))
                                              .toString(QStringLiteral("管理员操作失败"))));
                        return true;
                    });
                    if (!valid)
                        fail(QStringLiteral("管理员辅助通信无效，已停止"));
                });
            }
        });
    }
    void fail(const QString& message) {
        auto init = std::move(starting), done = std::move(completion);
        const auto receiver = context;
        q->stop();
        status = message;
        Q_EMIT q->changed();
        if (init)
            init(std::unexpected(message));
        if (done && receiver)
            done(std::unexpected(message));
    }
    void request(QJsonObject payload, QObject* receiver, Completion done) {
        if (!ready || !socket) {
            done(std::unexpected(QStringLiteral("管理员支持未连接，请在常规设置中重新启用")));
            return;
        }
        if (completion || !clicks.isEmpty()) {
            done(std::unexpected(QStringLiteral("上一个管理员操作尚未结束，本次操作已取消")));
            return;
        }
        requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        payload.insert(QStringLiteral("id"), requestId);
        payload.insert(QStringLiteral("deadline"), QString::number(GetTickCount64() + 300));
        completion = std::move(done);
        context = receiver;
        AllowSetForegroundWindow(childPid);
        send(socket, payload);
        deadline.start(2000);
    }
};
AdminBridge::AdminBridge(QObject* parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}
AdminBridge::~AdminBridge() {
    stop();
}
bool AdminBridge::ready() const {
    return d->ready;
}
bool AdminBridge::busy() const {
    return bool(d->completion) || bool(d->starting) || !d->clicks.isEmpty();
}
QString AdminBridge::status() const {
    return d->status;
}
bool AdminBridge::stop() {
    d->stopping = true;
    d->heartbeat.stop();
    d->deadline.stop();
    d->ready = false;
    d->clickDeadline.stop();
    d->clicks.clear();
    if (d->socket) {
        d->socket->disconnect(this);
        d->socket->abort();
        d->socket->deleteLater();
        d->socket = nullptr;
    }
    d->server.close();
    d->buffer.clear();
    const bool exited = !d->child || WaitForSingleObject(d->child.get(), 1500) == WAIT_OBJECT_0;
    if (exited) {
        d->child.reset();
        d->childPid = 0;
    }
    auto starting = std::move(d->starting), done = std::move(d->completion);
    const auto receiver = d->context;
    d->context = nullptr;
    d->status = exited ? QStringLiteral("未启用管理员支持")
                       : QStringLiteral("管理员辅助进程尚未退出，请稍后重试");
    d->stopping = false;
    Q_EMIT changed();
    if (starting)
        starting(std::unexpected(QStringLiteral("管理员支持已取消")));
    if (done && receiver)
        done(std::unexpected(QStringLiteral("管理员操作已取消")));
    return exited;
}
void AdminBridge::start(Completion done, bool preview) {
    if (ready()) {
        done({});
        return;
    }
    if (busy()) {
        done(std::unexpected(QStringLiteral("正在连接管理员辅助进程")));
        return;
    }
    if (!stop()) {
        done(std::unexpected(d->status));
        return;
    }
    d->preview = preview;
    const auto pipe =
        QStringLiteral("mt-admin-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d->server.listen(pipe)) {
        done(std::unexpected(d->server.errorString()));
        return;
    }
    d->starting = std::move(done);
    d->status = preview ? QStringLiteral("正在验证辅助连接")
                        : QStringLiteral("等待 Windows 管理员权限确认");
    Q_EMIT changed();
    const QStringList args{
        QStringLiteral("--admin-helper"),  pipe,
        QStringLiteral("--admin-parent"),  QString::number(GetCurrentProcessId()),
        QStringLiteral("--admin-started"), QString::number(currentProcessStarted())};
    if (preview) {
        auto testArgs = args;
        testArgs.append(QStringLiteral("--preview"));
        qint64 pid = 0;
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), testArgs, {}, &pid)) {
            d->fail(QStringLiteral("无法启动隔离辅助进程"));
            return;
        }
        d->childPid = DWORD(pid);
        d->child.reset(
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, d->childPid));
    } else {
        const auto path = QCoreApplication::applicationFilePath().toStdWString();
        const auto parameters =
            args.join(u' ').toStdWString(); // random pipe name and decimal identifiers only
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) {
            const auto code = GetLastError();
            d->fail(code == ERROR_CANCELLED
                        ? QStringLiteral("已取消管理员权限申请")
                        : QStringLiteral("无法启动管理员辅助进程（%1）").arg(code));
            return;
        }
        d->child.reset(info.hProcess);
        d->childPid = GetProcessId(info.hProcess);
    }
    if (!d->child) {
        d->fail(QStringLiteral("无法确认辅助进程身份"));
        return;
    }
    d->deadline.start(10000);
}
void AdminBridge::paste(WindowTarget target, QObject* context, Completion done) {
    d->request({{QStringLiteral("op"), QStringLiteral("paste")},
                {QStringLiteral("target"), targetJson(target)}},
               context, std::move(done));
}
void AdminBridge::suppressAltMenu(WindowTarget target, QObject* context, Completion done) {
    d->request({{QStringLiteral("op"), QStringLiteral("suppress_alt")},
                {QStringLiteral("target"), targetJson(target)}},
               context, std::move(done));
}
void AdminBridge::replayClick(WindowTarget target, quintptr foreground, QPoint position,
                              const QString& button) {
    if (!d->ready || !d->socket || d->clicks.size() >= 4 ||
        d->socket->bytesToWrite() > maxMessage) {
        Q_EMIT failed(QStringLiteral("管理员支持不可用，已取消点击回放"));
        return;
    }
    // Mouse clicks may arrive in a burst while an action response is pending.
    // Each complete pair is independent, bounded and revalidated by the helper.
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (d->clicks.isEmpty())
        d->clickDeadline.start(2000);
    d->clicks.insert(id);
    send(d->socket, {{QStringLiteral("op"), QStringLiteral("click")},
                     {QStringLiteral("target"), targetJson(target)},
                     {QStringLiteral("foreground"), QString::number(foreground)},
                     {QStringLiteral("x"), position.x()},
                     {QStringLiteral("y"), position.y()},
                     {QStringLiteral("button"), button},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("deadline"), QString::number(GetTickCount64() + 300)}});
}
void AdminBridge::gesture(const GestureAction& action, WindowTarget target, quintptr foreground,
                          QObject* context, Completion done) {
    d->request({{QStringLiteral("op"), QStringLiteral("gesture")},
                {QStringLiteral("target"), targetJson(target)},
                {QStringLiteral("foreground"), QString::number(foreground)},
                {QStringLiteral("action"), action.encode()}},
               context, std::move(done));
}
void AdminBridge::probe(QObject* context, Completion done) {
    d->request({{QStringLiteral("op"), QStringLiteral("probe")}}, context, std::move(done));
}
int runAdminHelper(const QString& pipe, quint32 parent, quint64 creation, bool preview) {
    if (!pipe.startsWith(QStringLiteral("mt-admin-")) || pipe.size() != 45 || !parent ||
        !creation || (!preview && !elevated(GetCurrentProcess())) || !sameSession(parent))
        return 2;
    wil::unique_handle owner(
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
    if (!owner || started(owner.get()) != creation || executable(owner.get()).isEmpty() ||
        executable(owner.get()).compare(executable(GetCurrentProcess()), Qt::CaseInsensitive) !=
            0 ||
        WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT)
        return 3;
    QLocalSocket socket;
    socket.setReadBufferSize(maxMessage * 2 + 1);
    socket.connectToServer(pipe);
    if (!socket.waitForConnected(5000))
        return 4;
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()),
                                     &server) ||
        server != parent)
        return 5;
    GestureActions actions;
    QByteArray buffer;
    quint64 lease = GetTickCount64();
    QObject::connect(&socket, &QLocalSocket::disconnected, qApp, [] { QCoreApplication::quit(); });
    QObject::connect(&socket, &QLocalSocket::readyRead, qApp, [&] {
        try {
            const bool valid = receive(&socket, buffer, [&](const QJsonObject& request) {
                lease = GetTickCount64();
                const auto operation = request.value(QStringLiteral("op")).toString();
                if (operation == QStringLiteral("ping")) {
                    send(&socket, {{QStringLiteral("pong"), true}});
                    return true;
                }
                const auto id = request.value(QStringLiteral("id")).toString();
                const auto expiry =
                    request.value(QStringLiteral("deadline")).toString().toULongLong();
                if (id.size() != 36 || expiry < lease || expiry - lease > 500)
                    return false;
                std::expected<void, QString> result;
                const auto target = targetFrom(request.value(QStringLiteral("target")).toObject());
                if (WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT)
                    return false;
                if (operation == QStringLiteral("probe")) { /* authenticated, no side effects */
                } else if (preview)
                    result = std::unexpected(QStringLiteral("隔离验证禁止执行输入或窗口操作"));
                else if (!target.processStarted || target.processId == parent ||
                         !sameSession(target.processId) || !DesktopServices::validTarget(target))
                    result = std::unexpected(QStringLiteral("目标窗口身份已失效，操作已取消"));
                else if (operation == QStringLiteral("paste"))
                    result = DesktopServices::sendPaste(target);
                else if (operation == QStringLiteral("click"))
                    result = replayClick(request, target);
                else if (operation == QStringLiteral("suppress_alt")) {
                    if (!DesktopServices::isForeground(target))
                        result = std::unexpected(QStringLiteral("目标窗口已改变，操作已取消"));
                    else
                        DesktopServices::suppressAltMenu();
                } else if (operation == QStringLiteral("gesture")) {
                    auto action =
                        GestureAction::parse(request.value(QStringLiteral("action")).toString());
                    if (!action || action->kind == GestureAction::Kind::Open ||
                        action->value == QStringLiteral("open_exe_dir"))
                        result = std::unexpected(QStringLiteral("管理员辅助进程拒绝此动作"));
                    else
                        result =
                            actions.execute(*action, target,
                                            quintptr(request.value(QStringLiteral("foreground"))
                                                         .toString()
                                                         .toULongLong()));
                } else
                    return false;
                if (operation == QStringLiteral("click")) {
                    send(&socket, {{QStringLiteral("click"), true},
                                   {QStringLiteral("id"), id},
                                   {QStringLiteral("ok"), bool(result)},
                                   {QStringLiteral("error"), result ? QString{} : result.error()}});
                } else
                    send(&socket, {{QStringLiteral("id"), id},
                                   {QStringLiteral("ok"), bool(result)},
                                   {QStringLiteral("error"), result ? QString{} : result.error()}});
                return true;
            });
            if (!valid)
                QCoreApplication::exit(6);
        } catch (...) {
            QCoreApplication::exit(7);
        }
    });
    QTimer lifetime;
    lifetime.setInterval(250);
    QObject::connect(&lifetime, &QTimer::timeout, qApp, [&] {
        if (WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT || GetTickCount64() - lease > 5000)
            QCoreApplication::quit();
    });
    lifetime.start();
    send(&socket, {{QStringLiteral("hello"), 1}, {QStringLiteral("preview"), preview}});
    return QCoreApplication::exec();
}
} // namespace mt
