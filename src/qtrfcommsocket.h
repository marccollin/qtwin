#pragma once

#include <QBluetoothSocket>

// Windows transport; the native Linux transport remains separate.
class QtRfcommSocket : public QBluetoothSocket
{
    Q_OBJECT

public:
    explicit QtRfcommSocket(QObject *parent = nullptr);

    void connectToDevice(const QBluetoothAddress &address, quint8 channel);
};
