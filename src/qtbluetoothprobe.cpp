#include <QBluetoothAddress>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    const QBluetoothAddress address(arguments.value(1));
    bool validDelay = true;
    const int sendDelayMs = arguments.size() == 3 ? arguments.at(2).toInt(&validDelay) : 0;
    if ((arguments.size() != 2 && arguments.size() != 3) || address.isNull()
        || !validDelay || sendDelayMs < 0 || sendDelayMs > 10000) {
        QTextStream(stderr) << "Usage: qtwin-bt-probe <Bluetooth address> [send delay in ms: 0-10000]\n";
        return 2;
    }

    QBluetoothSocket socket(QBluetoothServiceInfo::RfcommProtocol);
    QTimer timeout;
    timeout.setSingleShot(true);
    QByteArray response;
    bool receivedCount = false;
    bool finished = false;
    const auto finish = [&](int code, const QString &message) {
        if (finished)
            return;
        finished = true;
        timeout.stop();
        QTextStream(code == 0 ? stdout : stderr) << message << '\n';
        socket.abort();
        app.exit(code);
    };
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] {
        finish(1, QStringLiteral("Timed out waiting for %1.")
                      .arg(receivedCount ? QStringLiteral("STOP acknowledgement")
                                         : socket.state() == QBluetoothSocket::SocketState::ConnectedState
                                             ? QStringLiteral("DVTOT response")
                                             : QStringLiteral("UUID-based connection"))
                      + QStringLiteral(" Pending output: %1 bytes; received: %2 bytes.")
                            .arg(socket.bytesToWrite()).arg(response.size()));
    });
    QObject::connect(&socket, &QBluetoothSocket::connected, &app, [&] {
        QTextStream(stdout) << "Connected through Qt using the Serial Port UUID.\n"
                            << "Peer: " << socket.peerAddress().toString()
                            << "; channel: " << socket.peerPort() << '\n'
                            << "Waiting " << sendDelayMs << " ms before GTDL.\n";
        timeout.start(sendDelayMs + 5000);
        QTimer::singleShot(sendDelayMs, &app, [&] {
            if (finished)
                return;
            QTextStream(stdout) << "TX: GTDL\n";
            timeout.start(5000);
            const qint64 queued = socket.write("GTDL");
            QTextStream(stdout) << "Write accepted: " << queued << " bytes\n";
            if (queued != 4)
                finish(1, QStringLiteral("Could not queue GTDL: %1").arg(socket.errorString()));
        });
    });
    QObject::connect(&socket, &QBluetoothSocket::bytesWritten, &app, [&](qint64 count) {
        QTextStream(stdout) << "Qt reports bytes written: " << count << '\n';
    });
    QObject::connect(&socket, &QBluetoothSocket::readyRead, &app, [&] {
        const QByteArray chunk = socket.readAll();
        QTextStream(stdout) << "RX chunk (hex): " << chunk.toHex(' ') << '\n';
        response += chunk;
        if (!receivedCount) {
            const QByteArray prefix("DVTOT");
            const int prefixLength = qMin(response.size(), prefix.size());
            if (response.left(prefixLength) != prefix.left(prefixLength)) {
                finish(1, QStringLiteral("Unexpected response: %1").arg(QString::fromLatin1(response)));
                return;
            }
            if (response.size() < 10)
                return;
            const QByteArray digits = response.mid(5, 5);
            for (char digit : digits) {
                if (digit < '0' || digit > '9') {
                    finish(1, QStringLiteral("Invalid dive count: %1").arg(QString::fromLatin1(response)));
                    return;
                }
            }
            QTextStream(stdout) << "RX: " << response.left(10)
                                << "\nDive count: " << digits.toInt() << "\nTX: STOP\n";
            response.remove(0, 10);
            receivedCount = true;
            timeout.start(5000);
            if (socket.write("STOP") != 4) {
                finish(1, QStringLiteral("Could not queue STOP: %1").arg(socket.errorString()));
                return;
            }
        }
        if (response.isEmpty())
            return;
        if (response == "O")
            return;
        if (response == "OK")
            finish(0, QStringLiteral("RX: OK\nSuccess: Qt UUID connection and GTDL exchange verified."));
        else
            finish(1, QStringLiteral("Unexpected STOP response: %1").arg(QString::fromLatin1(response)));
    });
    QObject::connect(&socket, &QBluetoothSocket::errorOccurred, &app, [&](QBluetoothSocket::SocketError) {
        finish(1, QStringLiteral("Qt Bluetooth error: %1").arg(socket.errorString()));
    });
    QObject::connect(&socket, &QBluetoothSocket::disconnected, &app, [&] {
        finish(1, receivedCount
                      ? QStringLiteral("GTDL succeeded, but the device disconnected before acknowledging STOP.")
                      : QStringLiteral("Device disconnected before returning the dive count."));
    });

    QTextStream(stdout) << "Connecting to " << address.toString()
                        << " using UUID 00001101-0000-1000-8000-00805f9b34fb...\n";
    timeout.start(20000);
    //Defer connecting so synchronous backend errors are handled inside the event loop.
    QTimer::singleShot(0, &app, [&] {
        socket.connectToService(address, QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));
    });
    return app.exec();
}
