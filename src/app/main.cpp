#include "Application.h"
#include "infrastructure/i18n/I18n.h"
#include "BuildInfo.h"
#include "platform/windows/UpdateInstaller.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTimer>
#include <QTranslator>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTranslator chinese;
    if (chinese.load(QStringLiteral(":/mt/i18n/qtbase_zh_CN.qm")))
        app.installTranslator(&chinese);
    app.setOrganizationName(QStringLiteral("HunlongyuDevelopment"));
    app.setApplicationName(QStringLiteral("mt"));
    app.setApplicationDisplayName(QStringLiteral("MT"));
    app.setWindowIcon(QIcon(QStringLiteral(":/mt/logo.ico")));
    app.setApplicationVersion(QString::fromLatin1(TOOLS_VERSION));
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setFont(mt::DesktopServices::systemUiFont());
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("settings"), QStringLiteral("打开设置窗口")});
    QCommandLineOption shellHelper(QStringLiteral("shell-helper"));
    shellHelper.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(shellHelper);
    QCommandLineOption applyUpdate(QStringLiteral("apply-update"), QStringLiteral("安装已验证的更新"), QStringLiteral("plan"));
    applyUpdate.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(applyUpdate);
    QCommandLineOption updateComplete(QStringLiteral("update-complete"), QStringLiteral("更新启动确认"), QStringLiteral("stage"));
    updateComplete.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(updateComplete);
    parser.addVersionOption();
    parser.addOption({QStringLiteral("data-dir"),
                      QStringLiteral("独立开发数据目录"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("preview"),
         QStringLiteral("示例数据预览，不安装钩子、访问系统剪贴板或执行外部操作")});
    parser.addOption({QStringLiteral("evidence"),
                      QStringLiteral("在预览模式中保存界面截图和诊断"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("quit-after"),
                      QStringLiteral("在指定毫秒数后退出"),
                      QStringLiteral("milliseconds")});
    parser.process(app);
    if (parser.isSet(applyUpdate))
        return mt::runUpdateInstaller(parser.value(applyUpdate), parser.isSet(QStringLiteral("preview")));
    if (parser.isSet(QStringLiteral("shell-helper")))
        return mt::DesktopServices::runShellHelper();
    const bool preview = parser.isSet(QStringLiteral("preview"));
    auto root = parser.value(QStringLiteral("data-dir"));
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (preview)
            root = QDir(root).filePath(QStringLiteral("preview"));
    }
    root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    mt::DesktopServices desktop(root.toCaseFolded());
    if (!desktop.valid()) {
        QMessageBox::critical(nullptr, QStringLiteral("MT"),
                              mt::tx(u"无法建立应用实例服务"));
        return 2;
    }
    if (!desktop.primaryInstance()) {
        auto* retry = new QTimer(&app);
        retry->setInterval(100);
        int attempts = 0;
        QObject::connect(retry, &QTimer::timeout, &app, [&] {
            if (desktop.notifyPrimary()) {
                app.exit(0);
            } else if (++attempts >= 20)
                app.exit(3);
        });
        retry->start();
        return app.exec();
    }
    mt::Application application(root, preview, desktop);
    if (auto result = application.start(); !result) {
        QMessageBox::critical(nullptr, mt::tx(u"MT 无法启动"),
                              result.error() + mt::tx(u"\n原数据不会覆盖。"));
        return 2;
    }
    if (parser.isSet(QStringLiteral("settings")))
        QTimer::singleShot(0, &application, &mt::Application::showSettings);
    if (parser.isSet(updateComplete))
        mt::acknowledgeUpdateStartup(parser.value(updateComplete));
    if (parser.isSet(QStringLiteral("evidence"))) {
        if (!preview)
            return 4;
        QTimer::singleShot(1800, &application, [&] {
            application.saveEvidence(parser.value(QStringLiteral("evidence")));
        });
    }
    if (parser.isSet(QStringLiteral("quit-after"))) {
        bool ok = false;
        const int ms = parser.value(QStringLiteral("quit-after")).toInt(&ok);
        if (ok && ms >= 100)
            QTimer::singleShot(ms, &app, &QCoreApplication::quit);
    }
    const int result = app.exec();
    return result;
}
