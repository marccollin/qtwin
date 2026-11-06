#include "protocol.h"
#include "uddfwriter.h"

#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

using namespace qtwin;

namespace {

QString header(const QString &number = QStringLiteral("00012"), const QString &gas = QStringLiteral("03200"))
{
    //[7:12] number, [12:26] yyyyMMddHHmmss, [43:46] O2, [46:48] He
    QString record = QStringLiteral("DVSTART") + number + QStringLiteral("20261014153000");
    record += QString(43 - record.size(), u'0');
    return record + gas;
}

struct Element {
    QString name;
    QString text;
    QString ref;
};

QList<Element> readElements(const QByteArray &xml, QString *error)
{
    QList<Element> elements;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            Element element{reader.name().toString(), {}, reader.attributes().value(QLatin1String("ref")).toString()};
            if (element.name == QLatin1String("depth") || element.name == QLatin1String("temperature")
                || element.name == QLatin1String("divetime") || element.name == QLatin1String("o2")
                || element.name == QLatin1String("datetime") || element.name == QLatin1String("divenumber")
                || element.name == QLatin1String("greatestdepth")) {
                element.text = reader.readElementText();
            }
            elements << element;
        }
    }
    if (reader.hasError())
        *error = reader.errorString();
    return elements;
}

QStringList values(const QList<Element> &elements, const QString &name)
{
    QStringList result;
    for (const Element &element : elements) {
        if (element.name == name)
            result << (element.text.isEmpty() ? element.ref : element.text);
    }
    return result;
}

} //namespace

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void parsesDiveListEntry()
    {
        //Captured from a D-Twin.
        const auto entry = parseDiveListEntry(QStringLiteral("DV0029320261010140617016800300210012"));
        QVERIFY(entry);
        QCOMPARE(entry->id, QStringLiteral("00293"));
        QCOMPARE(entry->start, QDateTime(QDate(2026, 10, 10), QTime(14, 6, 17)));
        QCOMPARE(entry->durationSeconds, 28 * 60);
        QCOMPARE(entry->maxDepth, 21);
    }

    void rejectsShortDiveListEntry()
    {
        QVERIFY(!parseDiveListEntry(QStringLiteral("ENO")));
    }

    void convertsTemperatures()
    {
        QVERIFY(qFuzzyCompare(hundredthsCelsiusToKelvin(2150), 294.65));
        QVERIFY(qFuzzyCompare(hundredthsCelsiusToKelvin(944), 282.59));
    }

    void readsUnpaddedColdTemperatures()
    {
        //Captured sequence crossing 10 °C: the temperature loses a digit and
        //the following field shifts left.
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG0098901671024588"), std::nullopt), 1024);
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG0105901749446320"), 1024), 944);
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG0151001603043560"), 320), 304);
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG0205001071024685"), 912), 1024);
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG0105901749446320"), std::nullopt), 944);
        QCOMPARE(parseSampleTemperature(QStringLiteral("DVLOG01059017"), std::nullopt), std::optional<int>());
    }

    void readsPpo2AfterUnpaddedTemperature()
    {
        //The ppO2 (mBar, average of the O2 sensors) follows the temperature and
        //shifts left when the temperature has fewer digits.
        QString error;
        const auto dive = parseDive({header(),
                                     QStringLiteral("DVLOG0098901671024588"),
                                     QStringLiteral("DVLOG0105901749446320"),
                                     QStringLiteral("END")},
                                    &error);
        QVERIFY2(dive, qPrintable(error));
        QCOMPARE(dive->samples.at(0).ppo2Millibar, std::optional<int>(588));
        QCOMPARE(dive->samples.at(1).ppo2Millibar, std::optional<int>(6320));
    }

    void framesResponsesByLength()
    {
        QCOMPARE(expectedResponseLength("DVLOG0001"), 21);
        QCOMPARE(expectedResponseLength("DVSTART00"), 60);
        QCOMPARE(expectedResponseLength("DV00293"), 36);
        QCOMPARE(expectedResponseLength("DVNUM"), 5);
        QCOMPARE(expectedResponseLength("ENOD"), 4);
        QCOMPARE(expectedResponseLength("END0054"), 16);
        QCOMPARE(expectedResponseLength("DVSSW00006-024"), 14);
        QCOMPARE(expectedResponseLength("DVGSW00040050000"), 16);
        QCOMPARE(expectedResponseLength("??"), 0);
    }

    void parsesMetricDive()
    {
        QString error;
        const auto dive = parseDive({header(),
                                     QStringLiteral("DVLOG0001001232150311"),
                                     QStringLiteral("DVLOG0002002561900312"),
                                     QStringLiteral("DVGSW00002050000"),
                                     QStringLiteral("DVLOG0003000602000313"),
                                     QStringLiteral("END0003000250012")},
                                    &error);
        QVERIFY2(dive, qPrintable(error));
        QCOMPARE(dive->number, QStringLiteral("00012"));
        QCOMPARE(dive->start, QDateTime(QDate(2026, 10, 14), QTime(15, 30)));
        QCOMPARE(dive->mixes.size(), 2);
        QCOMPARE(dive->mixes.at(0).o2Percent, 32);
        QCOMPARE(dive->mixes.at(1).o2Percent, 50);
        QCOMPARE(dive->samples.size(), 3);
        QCOMPARE(dive->samples.at(1).timeSeconds, 20);
        QCOMPARE(dive->samples.at(1).depthMeters, 25.6);
        QVERIFY(qFuzzyCompare(*dive->samples.at(0).temperatureKelvin, 294.65));
        QCOMPARE(dive->samples.at(0).ppo2Millibar, std::optional<int>(311));
        QCOMPARE(dive->samples.at(2).ppo2Millibar, std::optional<int>(313));
        QCOMPARE(dive->gasSwitches.value(1, -1), 1);
        QVERIFY(dive->warnings.isEmpty());
    }

    void readsCapturedGasSwitch()
    {
        //Dive 267: rebreather (diluent air) bailout to EAN50, captured with btmon.
        QString error;
        const auto dive = parseDive({QStringLiteral("DVSTART0026720260620101701+010310631105007002100000516640102"),
                                     QStringLiteral("DVLOG0145302171616124"),
                                     QStringLiteral("DVLOG0145602151616124"),
                                     QStringLiteral("DVGSW00040050000"),
                                     QStringLiteral("DVLOG0146002151616123"),
                                     QStringLiteral("END0210000530026")},
                                    &error);
        QVERIFY2(dive, qPrintable(error));
        QCOMPARE(dive->mixes.size(), 2);
        QCOMPARE(dive->mixes.at(0).o2Percent, 21);
        QCOMPARE(dive->mixes.at(1).o2Percent, 50);
        QCOMPARE(dive->mixes.at(1).hePercent, 0);
        QCOMPARE(dive->samples.at(1).timeSeconds, 1456);
        QCOMPARE(dive->gasSwitches.value(1, -1), 1);
        QVERIFY(dive->warnings.isEmpty());
    }

    void splitsConcatenatedGasSwitch()
    {
        QCOMPARE(expectedResponseLength("DVGSW00040050000DVLOG0146002151616123"), 16);
    }

    void readsCapturedImperialModeDive()
    {
        //Dive 280 was recorded with the computer set to imperial units, yet
        //depth and temperature are transmitted in meters and °C.
        QString error;
        const auto dive = parseDive({QStringLiteral("DVSTART0028020260904193013+010310891105007001070000514720102"),
                                     QStringLiteral("DVLOG0000200141472305"),
                                     QStringLiteral("END0588001110024")},
                                    &error);
        QVERIFY2(dive, qPrintable(error));
        QCOMPARE(dive->mixes.at(0).o2Percent, 10);
        QCOMPARE(dive->mixes.at(0).hePercent, 70);
        QCOMPARE(dive->samples.at(0).depthMeters, 1.4);
        QVERIFY(qFuzzyCompare(*dive->samples.at(0).temperatureKelvin, 287.87));
    }

    void ignoresEventRecords()
    {
        QString error;
        const auto dive = parseDive({header(), QStringLiteral("DVLOG0018900262160238"), QStringLiteral("DVSSW00006-024"),
                                     QStringLiteral("DVLOG0019400272160238"), QStringLiteral("END0054000090008")},
                                    &error);
        QVERIFY2(dive, qPrintable(error));
        QCOMPARE(dive->samples.size(), 2);
        QVERIFY(dive->warnings.isEmpty());
    }

    void splitsFragmentedResponses()
    {
        const QStringList records = splitRecords(
            {header() + QStringLiteral("\r\nDVLOG0001001232150311"), QStringLiteral("\r\n"), QString::fromLatin1("END\0", 4)});
        QCOMPARE(records.size(), 3);
        QCOMPARE(records.at(1), QStringLiteral("DVLOG0001001232150311"));
        QCOMPARE(records.at(2), QStringLiteral("END"));
    }

    void reportsUnknownAndMalformedRecords()
    {
        QString error;
        const auto dive = parseDive({header(), QStringLiteral("XXXXX123"), QStringLiteral("DVLOGabc")},
                                    &error);
        QVERIFY(dive);
        QCOMPARE(dive->warnings.size(), 3); //malformed sample, unknown type, no samples
    }

    void rejectsShortHeader()
    {
        QString error;
        QVERIFY(!parseDive({QStringLiteral("DVSTART")}, &error));
        QVERIFY(!error.isEmpty());
    }

    void writesUddf()
    {
        QString error;
        const auto dive = parseDive({header(),
                                     QStringLiteral("DVLOG0001001232150311"),
                                     QStringLiteral("DVLOG0002002561900312"),
                                     QStringLiteral("DVGSW00002050000"),
                                     QStringLiteral("END")},
                                    &error);
        QVERIFY(dive);

        QString xmlError;
        const QList<Element> elements = readElements(writeUddf(*dive), &xmlError);
        QVERIFY2(xmlError.isEmpty(), qPrintable(xmlError));
        QCOMPARE(values(elements, QStringLiteral("o2")), QStringList({"0.32", "0.50"}));
        QCOMPARE(values(elements, QStringLiteral("divenumber")), QStringList({"12"}));
        QCOMPARE(values(elements, QStringLiteral("datetime")), QStringList({"2026-10-14T15:30:00"}));
        QCOMPARE(values(elements, QStringLiteral("depth")), QStringList({"12.30", "25.60"}));
        QCOMPARE(values(elements, QStringLiteral("temperature")), QStringList({"294.65", "292.15"}));
        QCOMPARE(values(elements, QStringLiteral("switchmix")), QStringList({"mix0", "mix1"}));
        QCOMPARE(values(elements, QStringLiteral("greatestdepth")), QStringList({"25.60"}));
    }

    void linksSingleMixToTank_data()
    {
        QTest::addColumn<int>("mixCount");
        QTest::newRow("no-mix") << 0;
        QTest::newRow("single-mix") << 1;
        QTest::newRow("multiple-mixes") << 2;
    }

    void linksSingleMixToTank()
    {
        QFETCH(int, mixCount);
        Dive dive;
        for (int i = 0; i < mixCount; ++i)
            dive.mixes << GasMix{21 + i, 0};
        QXmlStreamReader reader(writeUddf(dive));
        QStringList path;
        int tanks = 0;
        int links = 0;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                const QString name = reader.name().toString();
                if (name == QLatin1String("tankdata")) {
                    QCOMPARE(path.last(), QStringLiteral("dive"));
                    ++tanks;
                } else if (name == QLatin1String("link") && path.last() == QLatin1String("tankdata")) {
                    QCOMPARE(reader.attributes().value(QLatin1String("ref")).toString(), QStringLiteral("mix0"));
                    ++links;
                }
                QVERIFY(name != QLatin1String("tankvolume"));
                QVERIFY(name != QLatin1String("tankpressurebegin"));
                QVERIFY(name != QLatin1String("tankpressureend"));
                path << name;
            } else if (reader.isEndElement()) {
                path.removeLast();
            }
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QCOMPARE(tanks, mixCount == 1 ? 1 : 0);
        QCOMPARE(links, tanks);
    }

    void keepsStartingGasWhenSwitchingOnFirstSample()
    {
        QString error;
        const auto dive = parseDive({header(), QStringLiteral("DVLOG0001001232150311"), QStringLiteral("DVGSW00002050000")},
                                    &error);
        QVERIFY(dive);
        QString xmlError;
        const QList<Element> elements = readElements(writeUddf(*dive), &xmlError);
        QVERIFY2(xmlError.isEmpty(), qPrintable(xmlError));
        QCOMPARE(values(elements, QStringLiteral("switchmix")), QStringList({"mix0", "mix1"}));
        QCOMPARE(values(elements, QStringLiteral("depth")), QStringList({"12.30", "12.30"}));
    }

    void exportsFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ExportResult result =
            exportDive(QStringLiteral("00012"), {header(), QStringLiteral("DVLOG0001001232150311"), QStringLiteral("END")},
                       dir.path());
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.uddfPath.endsWith(QLatin1String("Dive00012_2026_10_14.uddf")));
        QVERIFY(QFile::exists(result.uddfPath));
        QVERIFY(QFile::exists(result.rawPath));
    }

    void keepsRawDataWhenParsingFails()
    {
        QTemporaryDir dir;
        const ExportResult result =
            exportDive(QStringLiteral("00012"), {QStringLiteral("garbage")}, dir.path());
        QVERIFY(!result.ok);
        QVERIFY(QFile::exists(result.rawPath));
        QVERIFY(result.uddfPath.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProtocol)
#include "tst_protocol.moc"
