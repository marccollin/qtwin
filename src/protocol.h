#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

//Record formats of the Bluedge D-Twin PC-connect protocol, recovered from the
//original D-Twin 3.0.366 application, a Bluetooth capture of it, and the
//D-Twin Bluetooth protocol document. Records have no terminator; field
//offsets are 0-based.
namespace qtwin {

//The computer always transmits metric data (depth in meters, temperature in
//°C), whatever display unit is set on it.
//One answer to GTNX, e.g. "DV0029320261010140617016800300210012".
struct DiveListEntry {
    QString id;               //5 characters; requested with "DV" + id
    QDateTime start;          //invalid if the date could not be parsed
    int durationSeconds = -1; //minute resolution, -1 if unknown
    int maxDepth = -1;        //whole meters, -1 if unknown
    QString raw;
};

struct Sample {
    int timeSeconds = 0;
    double depthMeters = 0.0;
    std::optional<double> temperatureKelvin;
    std::optional<int> ppo2Millibar; //average of the O2 sensors; CCR mode only
};

struct GasMix {
    int o2Percent = 21;
    int hePercent = 0;
};

enum class DiveMode {
    Unknown,
    OpenCircuit,
    ClosedCircuit,
    Gauge,
};

struct Dive {
    QString number;
    QDateTime start;
    QList<GasMix> mixes;            //mixes[0] is the starting mix
    QList<Sample> samples;
    QMap<int, int> gasSwitches;     //sample index -> mix index
    DiveMode mode = DiveMode::Unknown; //DVSTART mode digit: 0=OC, 1=CCR, 2=gauge
    std::optional<int> surfacePressureMillibar; //DVSTART ambient pressure
    std::optional<int> gradientFactorLow;       //DVSTART, percent (Bühlmann ZH-L16C only)
    std::optional<int> gradientFactorHigh;      //DVSTART, percent (Bühlmann ZH-L16C only)
    std::optional<int> averageDepthMeters;      //END record, whole meters
    QStringList warnings;
};

std::optional<DiveListEntry> parseDiveListEntry(const QString &response);

//Splits raw responses into trimmed, non-empty records.
QStringList splitRecords(const QStringList &responses);

//Parses the records received after "DV<id>" (first record = dive header).
std::optional<Dive> parseDive(const QStringList &responses, QString *error);

//Expected length of a complete response, or 0 if the record type is unknown.
qsizetype expectedResponseLength(const QByteArray &response);

//Reads the unpadded temperature (hundredths of a degree Celsius) of a DVLOG
//record. Below 10 °C it has fewer digits and the ppO2 field shifts left, so
//the width is ambiguous; the candidate closest to the previous value wins.
std::optional<int> parseSampleTemperature(const QString &record, std::optional<int> previousHundredths);

double hundredthsCelsiusToKelvin(int hundredths);

} //namespace qtwin
