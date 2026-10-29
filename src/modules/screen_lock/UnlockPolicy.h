#pragma once
#include <QByteArray>
#include <QString>
#include <expected>

namespace mt {
class UnlockPolicy {
  public:
    static bool validPassword(const QString& password);
    std::expected<void, QString> configure(bool required, const QString& password);
    bool append(QChar character);
    void backspace();
    void clear();
    bool submit();
    bool passwordRequired() const {
        return required_;
    }

  private:
    bool required_ = false;
    bool overflow_ = false;
    QByteArray digest_;
    QString input_;
};
} // namespace mt
