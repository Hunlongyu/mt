#include "infrastructure/updates/ReleaseInfo.h"
#include "infrastructure/updates/UpdateService.h"
#include "infrastructure/settings/Settings.h"
#include "platform/windows/UpdateInstaller.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cstring>

using namespace mt;
namespace {
QByteArray fixtureExe() {
    QByteArray bytes(256, '\0');
    bytes[0] = 'M'; bytes[1] = 'Z';
    qToLittleEndian<quint32>(128, bytes.data() + 60);
    bytes.replace(128, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(0x8664, bytes.data() + 132);
    return bytes;
}
QJsonObject release(const QByteArray& bytes = fixtureExe()) {
    QJsonObject asset{{QStringLiteral("name"), QStringLiteral("mt-windows-x64.exe")},
        {QStringLiteral("state"), QStringLiteral("uploaded")},
        {QStringLiteral("size"), bytes.size()},
        {QStringLiteral("digest"), QStringLiteral("sha256:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/Hunlongyu/mt/releases/download/v0.2.0/mt-windows-x64.exe")}};
    return {{QStringLiteral("tag_name"), QStringLiteral("v0.2.0")},
            {QStringLiteral("draft"), false}, {QStringLiteral("prerelease"), false},
            {QStringLiteral("assets"), QJsonArray{asset}}};
}
struct Response {
    int code = 200;
    QByteArray body;
    QUrl redirect;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    int delay = 1;
};
class Reply final : public QNetworkReply {
  public:
    Reply(const QNetworkRequest& request, Response response, QObject* parent)
        : QNetworkReply(parent), data_(std::move(response.body)) {
        setRequest(request); setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.code);
        if (!response.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        if (response.error != NoError) setError(response.error, QStringLiteral("fixture error"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(response.delay, this, [this] {
            if (isFinished()) return;
            Q_EMIT readyRead();
            if (isFinished()) return;
            setFinished(true);
            Q_EMIT finished();
        });
    }
    void abort() override { setFinished(true); }
    qint64 bytesAvailable() const override { return data_.size() - position_ + QNetworkReply::bytesAvailable(); }
  protected:
    qint64 readData(char* data, qint64 maximum) override {
        const auto count = qMin(maximum, static_cast<qint64>(data_.size()) - position_);
        if (count == 0) return -1;
        std::memcpy(data, data_.constData() + position_, static_cast<size_t>(count));
        position_ += count;
        return count;
    }
  private:
    QByteArray data_;
    qint64 position_ = 0;
};
class Network final : public QNetworkAccessManager {
  public:
    QList<Response> responses;
    QList<QUrl> requests;
  protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        requests.append(request.url());
        return new Reply(request, responses.isEmpty() ? Response{500, {}} : responses.takeFirst(), this);
    }
};
}
class UpdateTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void versionsAreSemanticAndStableOnly() {
        QVERIFY(stableVersion(QStringLiteral("v1.10.0")) > stableVersion(QStringLiteral("1.9.9")));
        for (const auto& value : {QStringLiteral("1.2"), QStringLiteral("01.2.3"),
                                  QStringLiteral("1.2.3-beta.1"), QStringLiteral("1.2.3/../x"),
                                  QStringLiteral("999999999999.0.0")})
            QVERIFY(!stableVersion(value));
        QVERIFY(parseUpdateRelease(QJsonDocument(release()).toJson(), QStringLiteral("0.1.0"))->has_value());
        QVERIFY(!parseUpdateRelease(QJsonDocument(release()).toJson(), QStringLiteral("0.2.0"))->has_value());
        QVERIFY(!parseUpdateRelease(QJsonDocument(release()).toJson(), QStringLiteral("1.0.0"))->has_value());
    }
    void rejectsUntrustedOrIncompleteMetadata() {
        const auto original = release();
        auto bad = original;
        bad.remove(QStringLiteral("draft"));
        QVERIFY(!parseUpdateRelease(QJsonDocument(bad).toJson(), QStringLiteral("0.1.0")));
        for (const auto& key : {QStringLiteral("digest"), QStringLiteral("size"), QStringLiteral("browser_download_url")}) {
            bad = original;
            auto asset = bad[QStringLiteral("assets")].toArray()[0].toObject();
            asset.remove(key);
            bad[QStringLiteral("assets")] = QJsonArray{asset};
            QVERIFY(!parseUpdateRelease(QJsonDocument(bad).toJson(), QStringLiteral("0.1.0")));
        }
        bad = original;
        bad[QStringLiteral("prerelease")] = true;
        QVERIFY(!parseUpdateRelease(QJsonDocument(bad).toJson(), QStringLiteral("0.1.0"))->has_value());
        bad = original;
        auto asset = bad[QStringLiteral("assets")].toArray()[0];
        bad[QStringLiteral("assets")] = QJsonArray{asset, asset};
        QVERIFY(!parseUpdateRelease(QJsonDocument(bad).toJson(), QStringLiteral("0.1.0")));
        QVERIFY(!parseUpdateRelease(QByteArray(1024 * 1024 + 1, 'x'), QStringLiteral("0.1.0")));
    }
    void redirectsCannotEscapeHttpsGitHubAssets() {
        for (const auto& value : {QStringLiteral("http://github.com/Hunlongyu/mt/releases/download/v1/mt.exe"),
                                  QStringLiteral("https://github.com.evil.example/Hunlongyu/mt/releases/download/x"),
                                  QStringLiteral("https://github.com/other/repo/releases/download/x"),
                                  QStringLiteral("https://user@github.com/Hunlongyu/mt/releases/download/x"),
                                  QStringLiteral("https://release-assets.githubusercontent.com:444/file")})
            QVERIFY(!isUpdateDownloadUrl(QUrl(value), true));
        const QUrl cdn(QStringLiteral("https://release-assets.githubusercontent.com/file?token=fixture"));
        QVERIFY(!isUpdateDownloadUrl(cdn));
        QVERIFY(isUpdateDownloadUrl(cdn, true));
    }
    void downloadIsVerifiedBeforeBecomingReady() {
        QTemporaryDir dir;
        Network network;
        const auto bytes = fixtureExe();
        network.responses = {{200, QJsonDocument(release(bytes)).toJson()},
            {302, {}, QUrl(QStringLiteral("https://release-assets.githubusercontent.com/fixture"))},
            {200, bytes}};
        UpdateService service(QStringLiteral("0.1.0"), dir.path(), nullptr, &network);
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Available);
        service.download();
        QTRY_COMPARE(service.state(), UpdateService::State::Ready);
        QFile downloaded(service.candidatePath());
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), bytes);
        QCOMPARE(network.requests.size(), 3);
        QVERIFY(verifyUpdateExecutable(service.candidatePath(), service.release()->sha256, bytes.size()));
    }
    void corruptionAndWrongLengthAreDiscarded() {
        for (const auto& bytes : {QByteArray(256, 'x'), QByteArray(257, 'x'), QByteArray(255, 'x')}) {
            QTemporaryDir dir;
            Network network;
            network.responses = {{200, QJsonDocument(release()).toJson()}, {200, bytes}};
            UpdateService service(QStringLiteral("0.1.0"), dir.path(), nullptr, &network);
            service.check();
            QTRY_COMPARE(service.state(), UpdateService::State::Available);
            service.download();
            QTRY_COMPARE(service.state(), UpdateService::State::Error);
            QVERIFY(service.candidatePath().isEmpty());
            QCOMPARE(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        }
    }
    void cancellationDoesNotAcceptLateCompletion() {
        QTemporaryDir dir;
        Network network;
        network.responses = {{200, QJsonDocument(release()).toJson()}, {200, fixtureExe(), {}, QNetworkReply::NoError, 100}};
        UpdateService service(QStringLiteral("0.1.0"), dir.path(), nullptr, &network);
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Available);
        service.download();
        service.cancel();
        QTest::qWait(150);
        QCOMPARE(service.state(), UpdateService::State::Available);
        QVERIFY(service.candidatePath().isEmpty());
    }
    void noReleaseAndRateLimitAreDifferent() {
        QTemporaryDir dir;
        Network network;
        network.responses = {{404, {}}, {403, {}}, {200, {}, {}, QNetworkReply::SslHandshakeFailedError}};
        UpdateService service(QStringLiteral("0.1.0"), dir.path(), nullptr, &network);
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Idle);
        QVERIFY(service.message().contains(QStringLiteral("没有")));
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Error);
        QVERIFY(service.message().contains(QStringLiteral("受限")));
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Error);
        QVERIFY(!service.release());
    }
    void hostileRedirectIsNotRequested() {
        QTemporaryDir dir;
        Network network;
        network.responses = {{200, QJsonDocument(release()).toJson()},
                             {302, {}, QUrl(QStringLiteral("https://evil.example/mt.exe"))}};
        UpdateService service(QStringLiteral("0.1.0"), dir.path(), nullptr, &network);
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Available);
        service.download();
        QTRY_COMPARE(service.state(), UpdateService::State::Error);
        QCOMPARE(network.requests.size(), 2);
    }
    void persistedPreferenceAndExecutableValidation() {
        auto settings = Settings::defaults();
        settings.autoCheckUpdates = false;
        auto decoded = Settings::decode(settings.encode());
        QVERIFY(decoded);
        QVERIFY(!decoded->autoCheckUpdates);
        auto object = QJsonDocument::fromJson(settings.encode()).object();
        object[QStringLiteral("autoCheckUpdates")] = QStringLiteral("false");
        QVERIFY(!Settings::decode(QJsonDocument(object).toJson()));
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("invalid.exe")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray invalid(256, 'x');
        file.write(invalid); file.close();
        QVERIFY(!verifyUpdateExecutable(file.fileName(), QCryptographicHash::hash(invalid, QCryptographicHash::Sha256), invalid.size()));
    }
};
QTEST_GUILESS_MAIN(UpdateTests)
#include "UpdateTests.moc"
