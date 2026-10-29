#pragma once
#include <QString>
#include <QStringView>
namespace mt {
// Chinese literal helper; safe on worker threads and independent of the OS locale.
QString tx(QStringView source);
}
