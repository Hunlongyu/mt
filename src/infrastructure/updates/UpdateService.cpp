#include "UpdateService.h"
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QSchannelBackend)

namespace mt {
namespace {
QNetworkRequest request(const QUrl& url) {
    QNetworkRequest result(url);
    result.setRawHeader("User-Agent", "MT-update-checker");
    result.setRawHeader("Accept-Encoding", "identity");
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    result.setTransferTimeout(15000);
    return result;
}
}
UpdateService::UpdateService(QString version, QString root, QObject* parent,
                             QNetworkAccessManager* network)
    : QObject(parent), version_(std::move(version)), cacheRoot_(std::move(root)),
      network_(network ? network : new QNetworkAccessManager(this)) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("更新请求超时，请稍后重试")); });
}
UpdateService::~UpdateService() { stopReply(); }
void UpdateService::setState(State state, QString message) {
    state_ = state;
    message_ = std::move(message);
    Q_EMIT changed();
}
void UpdateService::stopReply() {
    deadline_.stop();
    if (reply_) {
        auto* reply = reply_.data();
        reply_ = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}
void UpdateService::fail(QString message) {
    stopReply();
    file_.reset();
    staging_.reset();
    setState(State::Error, std::move(message));
}
void UpdateService::cancel() {
    if (!busy())
        return;
    stopReply();
    file_.reset();
    staging_.reset();
    setState(release_ ? State::Available : State::Idle, QStringLiteral("已取消更新操作"));
}
void UpdateService::check() {
    if (busy())
        return;
    release_.reset();
    staging_.reset();
    metadata_.clear();
    setState(State::Checking, QStringLiteral("正在检查 GitHub 正式版本…"));
    auto query = request(QUrl(QStringLiteral("https://api.github.com/repos/Hunlongyu/mt/releases/latest")));
    query.setRawHeader("Accept", "application/vnd.github+json");
    query.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    reply_ = network_->get(query);
    reply_->setReadBufferSize(64 * 1024);
    connect(reply_, &QIODevice::readyRead, this, [this] {
        metadata_ += reply_->readAll();
        if (metadata_.size() > 1024 * 1024)
            fail(QStringLiteral("版本信息超过大小限制"));
    });
    connect(reply_, &QNetworkReply::finished, this, [this] {
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply_->error();
        metadata_ += reply_->readAll();
        stopReply();
        if (code == 404) {
            setState(State::Idle, QStringLiteral("暂时没有可用的正式发布版本"));
            return;
        }
        if (networkError != QNetworkReply::NoError || code != 200) {
            fail(code == 403 || code == 429 ? QStringLiteral("GitHub 请求受限，请稍后重试")
                                           : QStringLiteral("无法连接 GitHub，请检查网络后重试"));
            return;
        }
        auto parsed = parseUpdateRelease(metadata_, version_);
        if (!parsed) { fail(parsed.error()); return; }
        release_ = *parsed;
        setState(release_ ? State::Available : State::Idle,
                 release_ ? QStringLiteral("发现新版本 %1，可下载更新").arg(release_->version)
                          : QStringLiteral("当前已是最新正式版本"));
    });
    deadline_.start(60000);
}
void UpdateService::download() {
    if (busy() || !release_ || state_ == State::Ready)
        return;
    if (!QDir().mkpath(cacheRoot_)) { fail(QStringLiteral("无法创建更新缓存目录")); return; }
    staging_ = std::make_unique<QTemporaryDir>(QDir(cacheRoot_).filePath(QStringLiteral("download-XXXXXX")));
    if (!staging_->isValid()) { fail(QStringLiteral("无法创建更新临时目录")); return; }
    file_ = std::make_unique<QSaveFile>(candidatePath());
    if (!file_->open(QIODevice::WriteOnly)) { fail(QStringLiteral("无法写入更新文件")); return; }
    hash_.reset();
    received_ = 0;
    percent_ = -1;
    setState(State::Downloading, QStringLiteral("正在下载更新 · 0%"));
    deadline_.start(10 * 60 * 1000);
    requestDownload(release_->url, 0);
}
QString UpdateService::candidatePath() const {
    return staging_ ? staging_->filePath(QStringLiteral("candidate.exe")) : QString{};
}
void UpdateService::readDownload() {
    if (!reply_ || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return;
    while (reply_ && reply_->bytesAvailable() > 0) {
        const auto chunk = reply_->read(64 * 1024);
        if (chunk.isEmpty()) break;
        received_ += chunk.size();
        if (received_ > release_->bytes || file_->write(chunk) != chunk.size()) {
            fail(QStringLiteral("更新文件超过声明大小或无法写入磁盘"));
            return;
        }
        hash_.addData(chunk);
    }
    const int progress = static_cast<int>(received_ * 100 / release_->bytes);
    if (progress != percent_) {
        percent_ = progress;
        setState(State::Downloading, QStringLiteral("正在下载更新 · %1%").arg(progress));
    }
}
void UpdateService::requestDownload(const QUrl& url, int redirects) {
    if (!isUpdateDownloadUrl(url, redirects > 0) || redirects > 5) {
        fail(QStringLiteral("更新下载重定向到不受信任的地址"));
        return;
    }
    reply_ = network_->get(request(url));
    reply_->setReadBufferSize(64 * 1024);
    connect(reply_, &QIODevice::readyRead, this, &UpdateService::readDownload);
    connect(reply_, &QNetworkReply::finished, this, [this, url, redirects] {
        readDownload();
        if (!reply_) return;
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply_->error();
        const auto target = reply_->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        reply_->deleteLater();
        reply_ = nullptr;
        if (code >= 300 && code < 400 && !target.isEmpty()) {
            requestDownload(url.resolved(target), redirects + 1);
            return;
        }
        if (code != 200 || networkError != QNetworkReply::NoError) {
            fail(QStringLiteral("下载失败，请检查网络后重试"));
            return;
        }
        if (received_ != release_->bytes || hash_.result() != release_->sha256) {
            fail(QStringLiteral("更新文件校验失败，已丢弃下载内容"));
            return;
        }
        if (!file_->commit()) { fail(QStringLiteral("保存更新文件失败")); return; }
        file_.reset();
        deadline_.stop();
        setState(State::Ready, QStringLiteral("版本 %1 已下载并通过校验，可以安装并重启").arg(release_->version));
    });
}
} // namespace mt
