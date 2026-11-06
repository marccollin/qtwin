#pragma once

#include "protocol.h"

#include <QBluetoothAddress>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#include "qtrfcommsocket.h"
using DTwinSocket = QtRfcommSocket;
#else
#include "rfcommsocket.h"
using DTwinSocket = RfcommSocket;
#endif

//Asynchronous client for the D-Twin RFCOMM protocol. Like the original
//application, every operation uses its own connection:
//dive list: GTDL, then GTNX repeatedly until ENOD
//one dive:  GTDV -> DVNUM, "DV<id>" -> header, GTNR repeatedly until END
//end:       STOP
class DTwinClient : public QObject
{
    Q_OBJECT

public:
    struct Timing {
        int connectTimeoutMs = 15000;
        int responseTimeoutMs = 3000; //the computer normally answers within 200 ms
        int quietGapMs = 300;        //fallback for responses of unknown length
        int commandDelayMs = 0;      //pause before each follow-up command
        int reconnectDelayMs = 1000; //pause between consecutive connections
    };

    explicit DTwinClient(QObject *parent = nullptr);
    ~DTwinClient() override;

    //The original application used channel 5 when the device name contains ':'.
    static quint16 channelForDeviceName(const QString &name);

    void setTarget(const QBluetoothAddress &address, quint16 channel);
    void setTiming(const Timing &timing);
    bool isBusy() const { return m_operation != Operation::None; }

    void fetchDiveList();
    void downloadDives(const QStringList &diveIds);
    void sendStop();
    void cancel();

signals:
    void diveListInfo(const QString &response);
    void diveListEntryReceived(const qtwin::DiveListEntry &entry);
    void diveListFinished(int count);
    void diveDownloadStarted(const QString &diveId, int index, int total);
    void diveRecordsReceived(const QString &diveId, int recordCount);
    void diveDownloaded(const QString &diveId, const QStringList &responses);
    void downloadsFinished();
    void stopSent();
    void failed(const QString &message);
    void traffic(const QString &direction, const QByteArray &data);

private:
    enum class Operation { None, ListInfo, ListEntries, Download, Stop };
    enum class Step { Idle, AwaitDvnum, AwaitHeader, AwaitRecord };

    void startOperation(Operation operation, int delayMs);
    void openConnection();
    void closeConnection();
    void onConnected();
    void onReadyRead();
    void onSocketDisconnected();
    void finishResponse();
    void handleResponse(const QString &response);
    void sendCommand(const QByteArray &command);
    void deliverRecords(const QStringList &records);
    void writePendingCommand();
    void waitForMoreResponses();
    void startNextDive(int delayMs);
    void finishStop();
    void fail(const QString &message);
    void finish();
    void later(int delayMs, void (DTwinClient::*step)());

    QBluetoothAddress m_address;
    quint16 m_channel = 1;
    Timing m_timing;

    QPointer<DTwinSocket> m_socket;
    QTimer m_connectTimer;
    QTimer m_responseTimer;
    QTimer m_quietTimer;
    QByteArray m_buffer;
    QByteArray m_pendingCommand;
    QByteArray m_lastCommand;
    bool m_expectingResponse = false;
    bool m_commandQueued = false;
    int m_retries = 0;
    QString m_lastRecord;
    quint64 m_generation = 0;

    Operation m_operation = Operation::None;
    Step m_step = Step::Idle;
    int m_listCount = 0;
    QStringList m_diveIds;
    int m_diveIndex = 0;
    QStringList m_responses;
};
