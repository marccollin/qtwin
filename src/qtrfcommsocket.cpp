#include "qtrfcommsocket.h"

QtRfcommSocket::QtRfcommSocket(QObject *parent)
    : QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, parent)
{
}

void QtRfcommSocket::connectToDevice(const QBluetoothAddress &address, quint8 channel)
{
    //Preserve the original application's channel selection (1 or 5).
    connectToService(address, channel);
}
