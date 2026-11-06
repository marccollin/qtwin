#include "uddfwriter.h"

#include <QDir>
#include <QSaveFile>
#include <QXmlStreamWriter>

#include <algorithm>

namespace qtwin {

namespace {

QString mixId(int index)
{
    return QStringLiteral("mix%1").arg(index);
}

QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

bool writeFile(const QString &path, const QByteArray &content, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} //namespace

QByteArray writeUddf(const Dive &dive)
{
    QByteArray output;
    QXmlStreamWriter xml(&output);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("uddf"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.streit.cc/uddf/3.2/"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("3.2.0"));

    xml.writeStartElement(QStringLiteral("generator"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("QTwin"));
    xml.writeStartElement(QStringLiteral("manufacturer"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("blue-edge"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Blue Edge Team"));
    xml.writeEndElement();
    xml.writeTextElement(QStringLiteral("type"), QStringLiteral("converter"));
    xml.writeTextElement(QStringLiteral("version"), QStringLiteral(QTWIN_VERSION));
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("gasdefinitions"));
    for (int i = 0; i < dive.mixes.size(); ++i) {
        const GasMix &mix = dive.mixes.at(i);
        xml.writeStartElement(QStringLiteral("mix"));
        xml.writeAttribute(QStringLiteral("id"), mixId(i));
        xml.writeTextElement(QStringLiteral("name"), QStringLiteral("%1/%2").arg(mix.o2Percent).arg(mix.hePercent));
        xml.writeTextElement(QStringLiteral("o2"), number(mix.o2Percent / 100.0, 2));
        xml.writeTextElement(QStringLiteral("he"), number(mix.hePercent / 100.0, 2));
        xml.writeEndElement();
    }
    xml.writeEndElement();

    const QString diveKey = dive.number.isEmpty() ? QStringLiteral("unknown") : dive.number;
    xml.writeStartElement(QStringLiteral("profiledata"));
    xml.writeStartElement(QStringLiteral("repetitiongroup"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("group_%1").arg(diveKey));
    xml.writeStartElement(QStringLiteral("dive"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("dive_%1").arg(diveKey));

    xml.writeStartElement(QStringLiteral("informationbeforedive"));
    bool numeric = false;
    const int diveNumber = dive.number.toInt(&numeric);
    xml.writeTextElement(QStringLiteral("divenumber"), numeric ? QString::number(diveNumber) : dive.number);
    if (dive.start.isValid())
        xml.writeTextElement(QStringLiteral("datetime"), dive.start.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
    xml.writeEndElement();

    if (dive.mixes.size() == 1) {
        xml.writeStartElement(QStringLiteral("tankdata"));
        xml.writeEmptyElement(QStringLiteral("link"));
        xml.writeAttribute(QStringLiteral("ref"), mixId(0));
        xml.writeEndElement();
    }

    xml.writeStartElement(QStringLiteral("samples"));
    const auto writeWaypoint = [&xml](const Sample &sample, std::optional<int> mix) {
        xml.writeStartElement(QStringLiteral("waypoint"));
        xml.writeTextElement(QStringLiteral("depth"), number(sample.depthMeters, 2));
        xml.writeTextElement(QStringLiteral("divetime"), QString::number(sample.timeSeconds));
        if (mix) {
            xml.writeEmptyElement(QStringLiteral("switchmix"));
            xml.writeAttribute(QStringLiteral("ref"), mixId(*mix));
        }
        if (sample.temperatureKelvin)
            xml.writeTextElement(QStringLiteral("temperature"), number(*sample.temperatureKelvin, 2));
        xml.writeEndElement();
    };
    for (int i = 0; i < dive.samples.size(); ++i) {
        const Sample &sample = dive.samples.at(i);
        const std::optional<int> switchedMix =
            dive.gasSwitches.contains(i) ? std::optional<int>(dive.gasSwitches.value(i)) : std::nullopt;
        if (i == 0) {
            //The first waypoint always declares the starting gas; a switch at the
            //same moment gets its own waypoint, as in the original application.
            writeWaypoint(sample, 0);
            if (switchedMix)
                writeWaypoint(sample, switchedMix);
        } else {
            writeWaypoint(sample, switchedMix);
        }
    }
    xml.writeEndElement();

    if (!dive.samples.isEmpty()) {
        const auto deepest = std::max_element(dive.samples.cbegin(), dive.samples.cend(),
                                              [](const Sample &a, const Sample &b) { return a.depthMeters < b.depthMeters; });
        xml.writeStartElement(QStringLiteral("informationafterdive"));
        xml.writeTextElement(QStringLiteral("greatestdepth"), number(deepest->depthMeters, 2));
        xml.writeTextElement(QStringLiteral("diveduration"), QString::number(dive.samples.last().timeSeconds));
        xml.writeEndElement();
    }

    xml.writeEndElement(); //dive
    xml.writeEndElement(); //repetitiongroup
    xml.writeEndElement(); //profiledata
    xml.writeEndElement(); //uddf
    xml.writeEndDocument();
    return output;
}

QString diveFileBaseName(const Dive &dive)
{
    QString base = QStringLiteral("Dive%1").arg(dive.number);
    if (dive.start.isValid())
        base += dive.start.toString(QStringLiteral("_yyyy_MM_dd"));
    return base;
}

ExportResult exportDive(const QString &diveId, const QStringList &responses,
                        const QString &outputDir)
{
    ExportResult result;
    if (!QDir().mkpath(outputDir)) {
        result.error = QStringLiteral("Cannot create folder %1").arg(outputDir);
        return result;
    }
    const QDir dir(outputDir);

    QString parseError;
    const std::optional<Dive> dive = parseDive(responses, &parseError);
    const QString base = dive ? diveFileBaseName(*dive) : QStringLiteral("Dive%1_unparsed").arg(diveId);

    QByteArray raw;
    for (const QString &response : responses)
        raw += response.toLatin1() + '\n';
    result.rawPath = dir.filePath(base + QStringLiteral(".txt"));
    if (!writeFile(result.rawPath, raw, &result.error)) {
        result.rawPath.clear();
        return result;
    }

    if (!dive) {
        result.error = parseError;
        return result;
    }
    result.warnings = dive->warnings;
    result.uddfPath = dir.filePath(base + QStringLiteral(".uddf"));
    if (!writeFile(result.uddfPath, writeUddf(*dive), &result.error)) {
        result.uddfPath.clear();
        return result;
    }
    result.ok = true;
    return result;
}

} //namespace qtwin
