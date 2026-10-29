#include <QtPlugin>

// Explicit imports keep static plugins reachable under /OPT:REF and LTO.
// Automatic SDK-wide plugin discovery is deliberately disabled in CMake.
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)
