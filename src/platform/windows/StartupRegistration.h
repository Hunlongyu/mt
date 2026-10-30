#pragma once
#include <QString>
#include <expected>
#include <optional>

namespace mt {
// Owns one value only. A different installation/data root is never overwritten.
class StartupRegistration {
  public:
    using Snapshot = std::optional<QString>;
    StartupRegistration(
        QString executable, QString dataRoot,
        QString key = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QString name = QStringLiteral("MT"));
    std::expected<Snapshot, QString> read() const;
    std::expected<void, QString> setEnabled(bool enabled) const;
    std::expected<void, QString> restore(const Snapshot& before, const Snapshot& expected) const;
    QString command() const {
        return command_;
    }

  private:
    std::expected<void, QString> write(const Snapshot&) const;
    QString command_, key_, name_;
};
} // namespace mt
