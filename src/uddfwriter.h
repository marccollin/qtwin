#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace qtwin {

QByteArray writeUddf(const Dive &dive);

//"Dive<number>_<yyyy>_<MM>_<dd>", matching the original application.
QString diveFileBaseName(const Dive &dive);

struct ExportResult {
    bool ok = false;
    QString rawPath;   //always written when possible, for troubleshooting
    QString uddfPath;
    QString error;
    QStringList warnings;
};

ExportResult exportDive(const QString &diveId, const QStringList &responses,
                        const QString &outputDir);

} //namespace qtwin
