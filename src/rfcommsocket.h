#pragma once

#include <QBluetoothAddress>
#include <QByteArray>
#include <QObject>

class QSocketNotifier;

// Non-blocking Linux RFCOMM socket connected to a fixed channel.
// QBluetoothSocket cannot do this with the BlueZ D-Bus backend that Qt uses
// for BlueZ >= 5.46 ("Connecting to port is not supported via Bluez DBus").
class RfcommSocket : public QObject
{
    Q_OBJECT

public:
    explicit RfcommSocket(QObject *parent = nullptr);
    ~RfcommSocket() override;

    void connectToDevice(const QBluetoothAddress &address, quint8 channel);
    qint64 write(const QByteArray &data);
    QByteArray readAll();
    void abort();
    QString errorString() const { return m_errorString; }

signals:
    void connected();
    void readyRead();
    void disconnected();
    void errorOccurred();

private:
    void onWritable();
    void onReadable();
    void setError(int errorNumber);

    int m_fd = -1;
    bool m_connected = false;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_readBuffer;
    QString m_errorString;
};
