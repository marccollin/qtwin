#include "rfcommsocket.h"

#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// From <bluetooth/bluetooth.h> and <bluetooth/rfcomm.h>, which are not
// required to be installed.
constexpr int BtProtoRfcomm = 3;

struct BdAddr {
    quint8 b[6];
} __attribute__((packed));

struct SockAddrRc {
    sa_family_t family;
    BdAddr address;
    quint8 channel;
};

} // namespace

RfcommSocket::RfcommSocket(QObject *parent)
    : QObject(parent)
{
}

RfcommSocket::~RfcommSocket()
{
    abort();
}

void RfcommSocket::connectToDevice(const QBluetoothAddress &address, quint8 channel)
{
    abort();
    m_errorString.clear();

    m_fd = ::socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, BtProtoRfcomm);
    if (m_fd < 0) {
        setError(errno);
        return;
    }

    SockAddrRc addr{};
    addr.family = AF_BLUETOOTH;
    const quint64 value = address.toUInt64();
    for (int i = 0; i < 6; ++i)
        addr.address.b[i] = quint8(value >> (8 * i)); // little-endian, as BlueZ expects
    addr.channel = channel;

    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    m_readNotifier->setEnabled(false);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &RfcommSocket::onReadable);
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &RfcommSocket::onWritable);

    if (::connect(m_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0 && errno != EINPROGRESS) {
        setError(errno);
        return;
    }
    // Completion (or failure) is reported through the write notifier.
}

void RfcommSocket::onWritable()
{
    if (m_connected || m_fd < 0)
        return;
    m_writeNotifier->setEnabled(false);

    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        error = errno;
    if (error != 0) {
        setError(error);
        return;
    }
    m_connected = true;
    m_readNotifier->setEnabled(true);
    emit connected();
}

void RfcommSocket::onReadable()
{
    if (m_fd < 0)
        return;
    char buffer[1024];
    bool gotData = false;
    int readError = 0;
    bool closedByPeer = false;
    for (;;) {
        const ssize_t n = ::read(m_fd, buffer, sizeof(buffer));
        if (n > 0) {
            m_readBuffer.append(buffer, n);
            gotData = true;
        } else if (n == 0) {
            closedByPeer = true;
            break;
        } else if (errno != EINTR) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                readError = errno;
            break;
        }
    }

    // Signal handlers may close this socket, so check the state after each one.
    if (gotData) {
        emit readyRead();
        if (m_fd < 0)
            return;
    }
    if (readError != 0) {
        setError(readError);
    } else if (closedByPeer) {
        abort();
        emit disconnected();
    }
}

qint64 RfcommSocket::write(const QByteArray &data)
{
    if (!m_connected || m_fd < 0)
        return -1;
    qint64 written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(m_fd, data.constData() + written, size_t(data.size() - written));
        if (n > 0) {
            written += n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ::usleep(1000); // commands are a few bytes; the send buffer is never full for long
        } else {
            setError(errno);
            return -1;
        }
    }
    return written;
}

QByteArray RfcommSocket::readAll()
{
    return std::exchange(m_readBuffer, {});
}

void RfcommSocket::abort()
{
    // deleteLater: abort() may run inside a notifier's activated() signal.
    for (QSocketNotifier *notifier : {m_readNotifier, m_writeNotifier}) {
        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
        }
    }
    m_readNotifier = nullptr;
    m_writeNotifier = nullptr;
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_connected = false;
    m_readBuffer.clear();
}

void RfcommSocket::setError(int errorNumber)
{
    m_errorString = QString::fromLocal8Bit(std::strerror(errorNumber));
    abort();
    emit errorOccurred();
}
