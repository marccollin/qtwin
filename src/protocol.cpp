#include "protocol.h"

#include <QRegularExpression>

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace qtwin {

namespace {

std::optional<int> intField(const QString &record, qsizetype pos, qsizetype len)
{
    if (record.size() < pos + len)
        return std::nullopt;
    bool ok = false;
    const int value = record.mid(pos, len).trimmed().toInt(&ok);
    return ok ? std::optional<int>(value) : std::nullopt;
}

} //namespace

double hundredthsCelsiusToKelvin(int hundredths)
{
    return hundredths / 100.0 + 273.15;
}

qsizetype expectedResponseLength(const QByteArray &response)
{
    struct Known { const char *prefix; qsizetype length; };
    static constexpr Known known[] = {
        {"DVSTART", 60}, {"DVLOG", 21}, {"DVSSW", 14}, {"DVGSW", 16}, {"DVNUM", 5},
        {"DVTOT", 10},   {"ENOD", 4},   {"END", 16},   {"OK", 2},
    };
    for (const Known &k : known) {
        if (response.startsWith(k.prefix))
            return k.length;
    }
    if (response.size() >= 3 && response.startsWith("DV") && std::isdigit(static_cast<unsigned char>(response.at(2))))
        return 36; //dive list entry
    return 0;
}

std::optional<int> parseSampleTemperature(const QString &record, std::optional<int> previousHundredths)
{
    constexpr qsizetype start = 14;
    if (record.size() <= start || record.at(start) < u'1' || record.at(start) > u'9')
        return std::nullopt;

    //4 digits: 10.00-49.99 °C, 3 digits: 1.00-9.99 °C, 2 digits: 0.10-0.99 °C.
    QList<int> candidates;
    for (qsizetype width = 4; width >= 2; --width) {
        if (const auto value = intField(record, start, width))
            candidates << *value;
    }
    candidates.removeIf([](int v) { return v > 4999; });
    if (candidates.isEmpty())
        return std::nullopt;
    if (!previousHundredths)
        return candidates.first();

    int best = candidates.first();
    for (int candidate : std::as_const(candidates)) {
        if (std::abs(candidate - *previousHundredths) < std::abs(best - *previousHundredths))
            best = candidate;
    }
    return best;
}

std::optional<DiveListEntry> parseDiveListEntry(const QString &response)
{
    //[2:7] dive id, [7:21] yyyyMMddHHmmss, [21:25] duration (units of 10 s),
    //[25:28] sample interval (s), [28:32] max depth, [32:36] average depth.
    const QString record = response.trimmed();
    if (record.size() < 7 || !record.startsWith(QLatin1String("DV")))
        return std::nullopt;

    DiveListEntry entry;
    entry.raw = record;
    entry.id = record.mid(2, 5);
    if (entry.id.trimmed().size() != 5)
        return std::nullopt;

    const QDate date = QDate::fromString(record.mid(7, 8), QStringLiteral("yyyyMMdd"));
    const QTime time = QTime::fromString(record.mid(15, 6), QStringLiteral("HHmmss"));
    if (date.isValid())
        entry.start = QDateTime(date, time.isValid() ? time : QTime(0, 0));
    if (const auto duration = intField(record, 21, 4))
        entry.durationSeconds = *duration * 10;
    if (const auto maxDepth = intField(record, 28, 4))
        entry.maxDepth = *maxDepth;
    return entry;
}

QStringList splitRecords(const QStringList &responses)
{
    static const QRegularExpression lineBreak(QStringLiteral("[\\r\\n]+"));
    QStringList records;
    for (const QString &response : responses) {
        QString cleaned = response;
        cleaned.remove(QChar(u'\0'));
        for (const QString &line : cleaned.split(lineBreak, Qt::SkipEmptyParts)) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty())
                records << trimmed;
        }
    }
    return records;
}

std::optional<Dive> parseDive(const QStringList &responses, QString *error)
{
    const QStringList records = splitRecords(responses);
    if (records.isEmpty()) {
        if (error)
            *error = QStringLiteral("No records received.");
        return std::nullopt;
    }

    //Header: [7:12] dive number, [12:26] yyyyMMddHHmmss, [43:46] O2 %, [46:48] He %.
    const QString &header = records.first();
    if (header.size() < 26) {
        if (error)
            *error = QStringLiteral("Dive header record is too short: \"%1\"").arg(header);
        return std::nullopt;
    }

    Dive dive;
    dive.number = header.mid(7, 5).trimmed();
    dive.start = QDateTime::fromString(header.mid(12, 14), QStringLiteral("yyyyMMddHHmmss"));
    if (!dive.start.isValid())
        dive.warnings << QStringLiteral("Could not read the dive date from \"%1\".").arg(header.mid(12, 14));

    GasMix firstMix;
    const auto o2 = intField(header, 43, 3);
    const auto he = intField(header, 46, 2);
    if (o2 && he) {
        firstMix.o2Percent = *o2;
        firstMix.hePercent = *he;
    } else {
        dive.warnings << QStringLiteral("Could not read the starting gas; assuming air.");
    }
    dive.mixes << firstMix;

    QMap<QString, int> unknownTypes;
    std::optional<int> previousTemperature;
    for (qsizetype i = 1; i < records.size(); ++i) {
        const QString &record = records.at(i);

        if (record.startsWith(QLatin1String("DVLOG"))) {
            //[5:10] time (s), [10:14] depth (tenths of a meter), [14:] temperature
            //(hundredths of °C, unpadded) followed by the ppO2 in mBar (average
            //of the O2 sensors; only meaningful in CCR mode, undefined otherwise).
            const auto time = intField(record, 5, 5);
            const auto depth = intField(record, 10, 4);
            if (!time || !depth) {
                dive.warnings << QStringLiteral("Skipped malformed sample \"%1\".").arg(record);
                continue;
            }
            Sample sample;
            sample.timeSeconds = *time;
            sample.depthMeters = *depth / 10.0;
            if (const auto temperature = parseSampleTemperature(record, previousTemperature)) {
                previousTemperature = temperature;
                sample.temperatureKelvin = hundredthsCelsiusToKelvin(*temperature);
                //The unpadded temperature shifts the ppO2 field left; it starts
                //right after the temperature digits.
                const qsizetype ppo2Start = 14 + qsizetype(std::to_string(*temperature).size());
                if (ppo2Start < record.size())
                    sample.ppo2Millibar = intField(record, ppo2Start, record.size() - ppo2Start);
            }
            dive.samples << sample;
        } else if (record.startsWith(QLatin1String("DVGSW"))) {
            //e.g. "DVGSW00040050000": [11:13] O2 %, [13:15] He %. The switch applies at
            //the previous sample (confirmed against the original app's UDDF of dive 267).
            const auto switchO2 = intField(record, 11, 2);
            const auto switchHe = intField(record, 13, 2);
            if (!switchO2 || !switchHe) {
                dive.warnings << QStringLiteral("Skipped malformed gas switch \"%1\".").arg(record);
                continue;
            }
            dive.mixes << GasMix{*switchO2, *switchHe};
            const int sampleIndex = dive.samples.isEmpty() ? 0 : int(dive.samples.size() - 1);
            dive.gasSwitches.insert(sampleIndex, int(dive.mixes.size() - 1));
        } else if (record.startsWith(QLatin1String("DVSSW"))) {
            //Setpoint switch record (CCR only): [5:10] runtime (s), then the O2
            //setpoint in mBar. Ignored; the observed "DVSSW00006-024" does not
            //match the documented unsigned 4-digit setpoint.
            continue;
        } else if (record.startsWith(QLatin1String("END"))) {
            break;
        } else {
            ++unknownTypes[record.left(5)];
        }
    }

    for (auto it = unknownTypes.cbegin(); it != unknownTypes.cend(); ++it)
        dive.warnings << QStringLiteral("Ignored %1 record(s) of unknown type \"%2\".").arg(it.value()).arg(it.key());
    if (dive.samples.isEmpty())
        dive.warnings << QStringLiteral("The dive contains no depth samples.");

    return dive;
}

} //namespace qtwin
