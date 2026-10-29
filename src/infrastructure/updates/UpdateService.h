#pragma once
#include "ReleaseInfo.h"
#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QTemporaryDir;
namespace mt {
class UpdateService final : public QObject {
    Q_OBJECT
  public:
    enum class State { Idle, Checking, Available, Downloading, Ready, Error };
    UpdateService(QString version, QString cacheRoot, QObject* parent = nullptr,
                  QNetworkAccessManager* network = nullptr);
    ~UpdateService() override;
    void check();
    void download();
    void cancel();
    State state() const { return state_; }
    QString message() const { return message_; }
    const std::optional<UpdateRelease>& release() const { return release_; }
    QString candidatePath() const;
    bool busy() const { return state_ == State::Checking || state_ == State::Downloading; }
  Q_SIGNALS:
    void changed();
  private:
    void setState(State state, QString message);
    void fail(QString message);
    void stopReply();
    void requestDownload(const QUrl& url, int redirects);
    void readDownload();
    QString version_, cacheRoot_, message_ = QStringLiteral("尚未检查更新");
    State state_ = State::Idle;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    QByteArray metadata_;
    std::optional<UpdateRelease> release_;
    std::unique_ptr<QTemporaryDir> staging_;
    std::unique_ptr<QSaveFile> file_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 received_ = 0;
    int percent_ = -1;
};
} // namespace mt
