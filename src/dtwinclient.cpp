#include "dtwinclient.h"

namespace {

constexpr int MaxDiveListEntries = 10000;
constexpr int MaxRecordsPerDive = 200000;
constexpr int MaxRetries = 3;

bool isRetryable(const QByteArray &command)
{
    //Commands that only ask for the next item; repeating them is harmless.
    return command == "GTDL" || command == "GTNX" || command == "GTNR";
}

} //namespace

DTwinClient::DTwinClient(QObject *parent)
    : QObject(parent)
{
    m_connectTimer.setSingleShot(true);
    m_responseTimer.setSingleShot(true);
    m_quietTimer.setSingleShot(true);

    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        fail(tr("Could not connect to the dive computer within %1 s. Is PC Connect active on the device?")
                 .arg(m_timing.connectTimeoutMs / 1000));
    });
    connect(&m_responseTimer, &QTimer::timeout, this, [this] {
        if (!m_buffer.isEmpty()) {
            finishResponse();
            return;
        }
        if (isRetryable(m_lastCommand) && m_retries < MaxRetries) {
            ++m_retries;
            emit traffic(QStringLiteral("!"), "no answer, sending " + m_lastCommand + " again");
            m_pendingCommand = m_lastCommand;
            writePendingCommand();
            return;
        }
        fail(tr("No answer to %1 within %2 s.")
                 .arg(QString::fromLatin1(m_lastCommand))
                 .arg(m_timing.responseTimeoutMs * (m_retries + 1) / 1000));
    });
    connect(&m_quietTimer, &QTimer::timeout, this, &DTwinClient::finishResponse);
}

DTwinClient::~DTwinClient()
{
    closeConnection();
}

quint16 DTwinClient::channelForDeviceName(const QString &name)
{
    return name.contains(u':') ? 5 : 1;
}

void DTwinClient::setTarget(const QBluetoothAddress &address, quint16 channel)
{
    m_address = address;
    m_channel = channel;
}

void DTwinClient::setTiming(const Timing &timing)
{
    m_timing = timing;
}

void DTwinClient::fetchDiveList()
{
    if (isBusy() || m_address.isNull()) {
        emit failed(isBusy() ? tr("Another operation is in progress.") : tr("No dive computer selected."));
        return;
    }
    m_listCount = 0;
    startOperation(Operation::ListInfo, 0);
}

void DTwinClient::downloadDives(const QStringList &diveIds)
{
    if (isBusy() || m_address.isNull() || diveIds.isEmpty()) {
        emit failed(isBusy() ? tr("Another operation is in progress.")
                             : m_address.isNull() ? tr("No dive computer selected.") : tr("No dive selected."));
        return;
    }
    m_diveIds = diveIds;
    m_diveIndex = 0;
    startNextDive(0);
}

void DTwinClient::sendStop()
{
    if (isBusy() || m_address.isNull()) {
        emit failed(isBusy() ? tr("Another operation is in progress.") : tr("No dive computer selected."));
        return;
    }
    startOperation(Operation::Stop, 0);
}

void DTwinClient::cancel()
{
    if (isBusy())
        fail(tr("Cancelled."));
}

void DTwinClient::startOperation(Operation operation, int delayMs)
{
    m_operation = operation;
    m_step = Step::Idle;
    later(delayMs, &DTwinClient::openConnection);
}

void DTwinClient::later(int delayMs, void (DTwinClient::*step)())
{
    if (delayMs <= 0) {
        (this->*step)();
        return;
    }
    const quint64 generation = m_generation;
    QTimer::singleShot(delayMs, this, [this, generation, step] {
        if (generation == m_generation)
            (this->*step)();
    });
}

void DTwinClient::openConnection()
{
    closeConnection();
    m_socket = new DTwinSocket(this);
    connect(m_socket, &DTwinSocket::connected, this, &DTwinClient::onConnected);
    connect(m_socket, &DTwinSocket::readyRead, this, &DTwinClient::onReadyRead);
    connect(m_socket, &DTwinSocket::disconnected, this, &DTwinClient::onSocketDisconnected);
    connect(m_socket, &DTwinSocket::errorOccurred, this, [this] {
        if (m_operation == Operation::Stop) {
            finishStop();
            return;
        }
        fail(tr("Bluetooth error: %1").arg(m_socket ? m_socket->errorString() : QString()));
    });

    emit traffic(QStringLiteral("•"),
                 QStringLiteral("connecting to %1, channel %2").arg(m_address.toString()).arg(m_channel).toLatin1());
    m_connectTimer.start(m_timing.connectTimeoutMs);
    m_socket->connectToDevice(m_address, quint8(m_channel));
}

void DTwinClient::closeConnection()
{
    m_connectTimer.stop();
    m_responseTimer.stop();
    m_quietTimer.stop();
    m_expectingResponse = false;
    m_commandQueued = false;
    m_retries = 0;
    m_buffer.clear();
    m_lastRecord.clear();
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void DTwinClient::onConnected()
{
    m_connectTimer.stop();
    emit traffic(QStringLiteral("•"), "connected");

    switch (m_operation) {
    case Operation::ListInfo:
        m_pendingCommand = "GTDL";
        writePendingCommand();
        break;
    case Operation::ListEntries:
        m_pendingCommand = "GTNX";
        writePendingCommand();
        break;
    case Operation::Download:
        m_step = Step::AwaitDvnum;
        m_pendingCommand = "GTDV";
        writePendingCommand();
        break;
    case Operation::Stop:
        emit traffic(QStringLiteral("→"), "STOP");
        m_socket->write("STOP");
        //Give the command time to leave before closing, as no answer is expected.
        later(500, &DTwinClient::finishStop);
        break;
    case Operation::None:
        closeConnection();
        break;
    }
}

void DTwinClient::sendCommand(const QByteArray &command)
{
    //Sent once all records of the current response have been handled.
    m_pendingCommand = command;
    m_commandQueued = true;
}

void DTwinClient::writePendingCommand()
{
    if (!m_socket)
        return;
    m_commandQueued = false;
    m_lastCommand = m_pendingCommand;
    m_expectingResponse = true;
    emit traffic(QStringLiteral("→"), m_lastCommand);
    m_socket->write(m_lastCommand);
    m_responseTimer.start(m_timing.responseTimeoutMs);
}

void DTwinClient::onReadyRead()
{
    if (!m_socket)
        return;
    const QByteArray data = m_socket->readAll();
    emit traffic(QStringLiteral("←"), data);
    if (!m_expectingResponse || data.isEmpty())
        return;

    m_buffer += data;
    m_responseTimer.start(m_timing.responseTimeoutMs);

    //Records have no terminator and may arrive split or several at once.
    QStringList records;
    for (;;) {
        while (!m_buffer.isEmpty() && (m_buffer.front() == '\r' || m_buffer.front() == '\n' || m_buffer.front() == '\0'))
            m_buffer.remove(0, 1);
        const qsizetype expected = qtwin::expectedResponseLength(m_buffer);
        if (expected == 0 || m_buffer.size() < expected)
            break;
        records << QString::fromLatin1(m_buffer.left(expected));
        m_buffer.remove(0, expected);
    }

    if (!records.isEmpty()) {
        deliverRecords(records);
        return;
    }
    if (m_buffer.isEmpty())
        return;
    const char last = m_buffer.back();
    if (qtwin::expectedResponseLength(m_buffer) == 0 && (last == '\n' || last == '\r' || last == '\0'))
        finishResponse();
    else
        m_quietTimer.start(m_timing.quietGapMs);
}

void DTwinClient::onSocketDisconnected()
{
    if (m_operation == Operation::Stop) {
        finishStop();
        return;
    }
    if (m_operation != Operation::None)
        fail(tr("The dive computer closed the connection."));
}

void DTwinClient::finishResponse()
{
    //Fallback for data of unknown length: treat everything received as one response.
    QString response = QString::fromLatin1(m_buffer);
    m_buffer.clear();
    response.remove(QChar(u'\0'));
    response = response.trimmed();
    if (response.isEmpty()) {
        waitForMoreResponses();
        return;
    }
    deliverRecords({response});
}

void DTwinClient::deliverRecords(const QStringList &records)
{
    m_quietTimer.stop();
    m_responseTimer.stop();
    m_expectingResponse = false;
    m_retries = 0;
    m_commandQueued = false;

    const quint64 generation = m_generation;
    for (const QString &record : records) {
        if (record == m_lastRecord) {
            //A request repeated after a timeout can return the same record twice.
            emit traffic(QStringLiteral("!"), "ignoring repeated record " + record.toLatin1());
            continue;
        }
        m_lastRecord = record;
        handleResponse(record);
        if (generation != m_generation || !m_socket)
            return; //finished, failed or reconnecting
    }

    if (m_commandQueued)
        later(m_timing.commandDelayMs, &DTwinClient::writePendingCommand);
    else if (!m_expectingResponse)
        waitForMoreResponses();
}

void DTwinClient::waitForMoreResponses()
{
    m_expectingResponse = true;
    m_responseTimer.start(m_timing.responseTimeoutMs);
}

void DTwinClient::handleResponse(const QString &response)
{
    switch (m_operation) {
    case Operation::ListInfo:
        emit diveListInfo(response);
        closeConnection();
        startOperation(Operation::ListEntries, m_timing.reconnectDelayMs);
        return;

    case Operation::ListEntries: {
        bool done = false;
        for (const QString &line : qtwin::splitRecords({response})) {
            if (line.contains(QLatin1String("ENOD"))) {
                done = true;
                break;
            }
            if (const auto entry = qtwin::parseDiveListEntry(line)) {
                ++m_listCount;
                emit diveListEntryReceived(*entry);
            } else {
                emit traffic(QStringLiteral("!"), "unreadable dive list entry: " + line.toLatin1());
            }
        }
        if (done) {
            const int count = m_listCount;
            finish();
            emit diveListFinished(count);
        } else if (m_listCount >= MaxDiveListEntries) {
            fail(tr("The dive list did not end after %1 entries.").arg(MaxDiveListEntries));
        } else {
            sendCommand("GTNX");
        }
        return;
    }

    case Operation::Download: {
        const QString diveId = m_diveIds.at(m_diveIndex);
        switch (m_step) {
        case Step::AwaitDvnum:
            if (response.contains(QLatin1String("DVNUM"))) {
                m_step = Step::AwaitHeader;
                sendCommand("DV" + diveId.toLatin1());
            } else {
                waitForMoreResponses();
            }
            return;
        case Step::AwaitHeader:
            m_responses << response;
            m_step = Step::AwaitRecord;
            emit diveRecordsReceived(diveId, int(m_responses.size()));
            sendCommand("GTNR");
            return;
        case Step::AwaitRecord:
            m_responses << response;
            emit diveRecordsReceived(diveId, int(m_responses.size()));
            if (response.startsWith(QLatin1String("END"))) {
                closeConnection();
                const QStringList responses = m_responses;
                ++m_diveIndex;
                emit diveDownloaded(diveId, responses);
                startNextDive(m_timing.reconnectDelayMs);
            } else if (m_responses.size() >= MaxRecordsPerDive) {
                fail(tr("Dive %1 did not end after %2 records.").arg(diveId).arg(MaxRecordsPerDive));
            } else {
                sendCommand("GTNR");
            }
            return;
        case Step::Idle:
            return;
        }
        return;
    }

    case Operation::Stop:
    case Operation::None:
        return;
    }
}

void DTwinClient::startNextDive(int delayMs)
{
    if (m_operation == Operation::None && m_diveIndex > 0)
        return; //cancelled while the previous dive was being saved
    if (m_diveIndex >= m_diveIds.size()) {
        finish();
        emit downloadsFinished();
        return;
    }
    m_responses.clear();
    emit diveDownloadStarted(m_diveIds.at(m_diveIndex), m_diveIndex, int(m_diveIds.size()));
    startOperation(Operation::Download, delayMs);
}

void DTwinClient::finishStop()
{
    if (m_operation != Operation::Stop)
        return;
    finish();
    emit stopSent();
}

void DTwinClient::fail(const QString &message)
{
    finish();
    emit failed(message);
}

void DTwinClient::finish()
{
    closeConnection();
    ++m_generation;
    m_operation = Operation::None;
    m_step = Step::Idle;
}
