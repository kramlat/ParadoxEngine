// Network play over Qt's WebSockets: see Backend/NetPlay.h for what it is for and how a game uses it. No QObject subclasses (so no moc): the sockets' signals go to lambdas that keep to the NetPlay they belong to.
//
// The conversation. After the WebSocket opens the host sends text (JSON): hello {game, nonce}. The guest answers auth {cn, mac}; mac = HMAC-SHA256(key, "guest" game nonce cn), where key = PBKDF2-HMAC-SHA256
// of the password (salt: the host's nonce). The host checks it and answers ok {mac} (HMAC(key, "host" game nonce cn): the proof that it knows the password too) or deny {why}. Both then make the session key
// HMAC(key, "session" nonce cn). From then on every message is binary: type, a count (it goes up by one in each direction), the payload and the first 8 bytes of HMAC(session key, type count payload).
#include "../NetPlay.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QPointer>
#include <QMessageAuthenticationCode>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <map>
#include <string>
#include <vector>

namespace {

enum MessageType : uint8_t { Msg_Input = 1, Msg_Hash = 2, Msg_Start = 3, Msg_Ping = 4, Msg_Pong = 5, Msg_Bye = 6 };

const int kPbkdf2Iterations = 10000;
const int kMacBytes = 8;
const int kMaxStartData = 4096;
const int kMaxMessage = 8192;
const uint32_t kHashEvery = 60;
const int kAuthTimeoutMs = 10000;
const int kMaxFailures = 5;
const int kBlockMs = 30000;

}

struct NetPlay {
    NetPlayStatus status = NETPLAY_IDLE;
    bool host = false;
    std::string game, password, reason;
    int delay = 4;
    int ping_ms = -1;

    QWebSocketServer *server = nullptr;
    QPointer<QWebSocket> socket;    // the other side, once it is there (host: once it has proved itself)
    QPointer<QWebSocket> pending;   // host: someone who has not proved it yet
    QTimer *auth_timer = nullptr;
    QTimer *ping_timer = nullptr;
    QByteArray nonce, client_nonce, key, session_key;
    int failures = 0;
    QElapsedTimer blocked;
    bool is_blocked = false;

    uint16_t discovery_port = NETPLAY_DISCOVERY_PORT;
    QUdpSocket *announce_socket = nullptr; // host: sends the announcements
    QTimer *announce_timer = nullptr;
    QUdpSocket *search_socket = nullptr;   // guest: hears them
    QByteArray search_tag;
    bool found = false;                    // the guest has heard its host: NetPlay_Poll joins it (not inside the socket's own signal)
    QString found_host;
    uint16_t found_port = 0;

    uint32_t send_seq = 0, recv_seq = 0;
    uint32_t frame = 0;
    uint32_t sent_through = 0; // frames up to this one (not including) have had their local input sent
    std::map<uint32_t, uint8_t> local_inputs, remote_inputs;
    std::map<uint32_t, uint32_t> local_hashes, remote_hashes;
    uint32_t hash_reported = 0;
    bool desync = false;
    uint32_t desync_frame = 0;
    std::vector<uint8_t> start_data;
};

namespace {

void EnsureApplication() {
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char name[] = "paradox";
    static char *argv[] = { name, nullptr };
    static QCoreApplication app(argc, argv); // (a game without a Qt application of its own, or a test)
}

QByteArray Normalize(const std::string &password) {
    QByteArray bytes;
    for (char c : password) {
        if (c == '-' || c == ' ' || c == '\t')
            continue;
        bytes.append((char)((c >= 'a' && c <= 'z') ? c - 32 : c));
    }
    return bytes;
}

std::string MakePassword() {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; // (36 symbols, the 8 by 5 grid of the code screen: the user's codes have O and 0 both)
    std::string out;
    for (int i = 0; i < 20; i++) {
        if (i > 0 && i % 5 == 0)
            out += '-';
        out += alphabet[QRandomGenerator::system()->bounded(36)];
    }
    return out;
}

// The tag of a session code in the announcements: 8 bytes of a hash of the game and the code, in hex (the code is 100 bits: this does not give it away)
QByteArray SessionTag(const std::string &game, const std::string &code) {
    return QCryptographicHash::hash(QByteArray("paradox-netplay-announce|") + QByteArray::fromStdString(game) + '|' + Normalize(code), QCryptographicHash::Sha256).left(8).toHex();
}

QByteArray Random16() {
    QByteArray bytes(16, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), 4);
    return bytes;
}

QByteArray Hmac(const QByteArray &key, const QByteArray &data) {
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

bool SameBytes(const QByteArray &a, const QByteArray &b) { // (the time does not tell where they differ)
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

QByteArray ProofData(const char *who, const NetPlay *n) {
    return QByteArray(who) + QByteArray::fromStdString(n->game) + n->nonce + n->client_nonce;
}

void SetKeys(NetPlay *n, const QByteArray &password) {
    n->key = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password, n->nonce, kPbkdf2Iterations, 32);
}

void SetSessionKey(NetPlay *n) {
    n->session_key = Hmac(n->key, ProofData("session", n));
}

void Close(NetPlay *n, const std::string &reason, bool tell = false, const char *bye = nullptr);

void Detach(NetPlay *n) {
    if (n->auth_timer != nullptr) {
        n->auth_timer->stop();
    }
    if (n->ping_timer != nullptr) {
        n->ping_timer->stop();
    }
}

QByteArray Pack(NetPlay *n, uint8_t type, const QByteArray &payload) {
    QByteArray head;
    head.append((char)type);
    const uint32_t seq = ++n->send_seq;
    for (int i = 0; i < 4; i++)
        head.append((char)((seq >> (8 * i)) & 0xFF));
    const QByteArray mac = Hmac(n->session_key, head + payload).left(kMacBytes);
    return head + payload + mac;
}

void Send(NetPlay *n, uint8_t type, const QByteArray &payload) {
    if (n->socket == nullptr)
        return;
    n->socket->sendBinaryMessage(Pack(n, type, payload));
}

QByteArray U32(uint32_t v) {
    QByteArray out;
    for (int i = 0; i < 4; i++)
        out.append((char)((v >> (8 * i)) & 0xFF));
    return out;
}

uint32_t ReadU32(const QByteArray &data, int at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)(unsigned char)data[at + i] << (8 * i);
    return v;
}

void SendText(QWebSocket *socket, const QJsonObject &object) {
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CheckHashes(NetPlay *n, uint32_t frame) {
    auto local = n->local_hashes.find(frame);
    auto remote = n->remote_hashes.find(frame);
    if (local == n->local_hashes.end() || remote == n->remote_hashes.end())
        return;
    if (local->second != remote->second && !n->desync) {
        n->desync = true;
        n->desync_frame = frame;
        Close(n, "the two games differ (desync) at frame " + std::to_string(frame), true, "desync");
    }
    n->local_hashes.erase(frame);
    n->remote_hashes.erase(frame);
}

void HandleBinary(NetPlay *n, const QByteArray &data) {
    if (data.size() < 1 + 4 + kMacBytes || data.size() > kMaxMessage) {
        Close(n, "a message of the wrong size", true, "bad message");
        return;
    }
    const int body = data.size() - kMacBytes;
    if (!SameBytes(Hmac(n->session_key, data.left(body)).left(kMacBytes), data.mid(body))) {
        Close(n, "a message that has been changed on the way", true, "bad mac");
        return;
    }
    const uint32_t seq = ReadU32(data, 1);
    if (seq != n->recv_seq + 1) {
        Close(n, "a message out of order or sent again", true, "bad order");
        return;
    }
    n->recv_seq = seq;
    const uint8_t type = (uint8_t)data[0];
    const QByteArray payload = data.mid(5, body - 5);
    switch (type) {
    case Msg_Input:
        if (payload.size() == 5 && n->status == NETPLAY_RUNNING) {
            const uint32_t frame = ReadU32(payload, 0);
            if (frame >= n->frame)
                n->remote_inputs[frame] = (uint8_t)payload[4];
        }
        break;
    case Msg_Hash:
        if (payload.size() == 8 && n->status == NETPLAY_RUNNING) {
            const uint32_t frame = ReadU32(payload, 0);
            n->remote_hashes[frame] = ReadU32(payload, 4);
            CheckHashes(n, frame);
        }
        break;
    case Msg_Start:
        if (!n->host && n->status == NETPLAY_READY && payload.size() <= kMaxStartData) {
            n->start_data.assign(payload.begin(), payload.end());
            n->status = NETPLAY_RUNNING;
            n->frame = 0;
        }
        break;
    case Msg_Ping:
        Send(n, Msg_Pong, payload);
        break;
    case Msg_Pong:
        if (payload.size() == 4) {
            const uint32_t sent = ReadU32(payload, 0);
            const uint32_t now = (uint32_t)(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
            n->ping_ms = (int)(now - sent);
        }
        break;
    case Msg_Bye:
        Close(n, payload.isEmpty() ? std::string("the other side ended the session") : "the other side ended the session (" + payload.toStdString() + ")");
        break;
    default:
        break;
    }
}

void StartPinging(NetPlay *n) {
    if (n->ping_timer == nullptr) {
        n->ping_timer = new QTimer();
        QObject::connect(n->ping_timer, &QTimer::timeout, [n]() {
            if (n->socket != nullptr && (n->status == NETPLAY_READY || n->status == NETPLAY_RUNNING))
                Send(n, Msg_Ping, U32((uint32_t)(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF)));
        });
    }
    n->ping_timer->start(2000);
}

void Authenticated(NetPlay *n, QWebSocket *socket) {
    n->socket = socket;
    n->pending = nullptr;
    n->auth_timer->stop();
    SetSessionKey(n);
    n->send_seq = n->recv_seq = 0;
    n->status = NETPLAY_READY;
    QObject::connect(socket, &QWebSocket::binaryMessageReceived, [n](const QByteArray &data) { HandleBinary(n, data); });
    QObject::connect(socket, &QWebSocket::disconnected, [n, socket]() {
        if (n->socket == socket && n->status != NETPLAY_CLOSED)
            Close(n, "the other side has gone");
    });
    StartPinging(n);
}

// The host's side of the conversation: what a connection that has just come says
void HostHandleText(NetPlay *n, QWebSocket *socket, const QString &text) {
    if (socket != n->pending)
        return;
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    if (message.value("t").toString() != "auth") {
        socket->close();
        return;
    }
    n->client_nonce = QByteArray::fromHex(message.value("cn").toString().toLatin1());
    const QByteArray mac = QByteArray::fromHex(message.value("mac").toString().toLatin1());
    if (n->client_nonce.size() != 16 || !SameBytes(mac, Hmac(n->key, ProofData("guest", n)))) {
        n->failures++;
        if (n->failures >= kMaxFailures) {
            n->is_blocked = true;
            n->blocked.start();
        }
        SendText(socket, QJsonObject{ { "t", "deny" }, { "why", "the password is wrong" } });
        n->pending = nullptr;
        socket->close();
        return;
    }
    SendText(socket, QJsonObject{ { "t", "ok" }, { "mac", QString::fromLatin1(Hmac(n->key, ProofData("host", n)).toHex()) }, { "delay", n->delay } });
    Authenticated(n, socket);
}

void HostNewConnection(NetPlay *n) {
    while (n->server->hasPendingConnections()) {
        QWebSocket *socket = n->server->nextPendingConnection();
        QObject::connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
        if (n->is_blocked && n->blocked.elapsed() < kBlockMs) {
            socket->close();
            continue;
        }
        n->is_blocked = false;
        if (n->socket != nullptr || n->pending != nullptr) {
            SendText(socket, QJsonObject{ { "t", "deny" }, { "why", "the session already has its guest" } });
            socket->close();
            continue;
        }
        n->pending = socket;
        n->nonce = Random16();
        SetKeys(n, Normalize(n->password));
        QObject::connect(socket, &QWebSocket::textMessageReceived, [n, socket](const QString &text) { HostHandleText(n, socket, text); });
        QObject::connect(socket, &QWebSocket::disconnected, [n, socket]() {
            if (n->pending == socket)
                n->pending = nullptr;
        });
        SendText(socket, QJsonObject{ { "t", "hello" }, { "v", 1 }, { "game", QString::fromStdString(n->game) }, { "nonce", QString::fromLatin1(n->nonce.toHex()) } });
        n->auth_timer->start(kAuthTimeoutMs);
    }
}

// The guest's side
void GuestHandleText(NetPlay *n, const QString &text) {
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = message.value("t").toString();
    if (type == "hello" && n->status == NETPLAY_CONNECTING && n->nonce.isEmpty()) {
        if (message.value("game").toString().toStdString() != n->game) {
            Close(n, "the host is playing a different game or version (" + message.value("game").toString().toStdString() + ")");
            return;
        }
        n->nonce = QByteArray::fromHex(message.value("nonce").toString().toLatin1());
        if (n->nonce.size() != 16) {
            Close(n, "the host's greeting is wrong");
            return;
        }
        n->client_nonce = Random16();
        SetKeys(n, Normalize(n->password));
        SendText(n->socket, QJsonObject{ { "t", "auth" }, { "cn", QString::fromLatin1(n->client_nonce.toHex()) }, { "mac", QString::fromLatin1(Hmac(n->key, ProofData("guest", n)).toHex()) } });
    } else if (type == "ok" && n->status == NETPLAY_CONNECTING && !n->nonce.isEmpty()) {
        if (!SameBytes(QByteArray::fromHex(message.value("mac").toString().toLatin1()), Hmac(n->key, ProofData("host", n)))) {
            Close(n, "the host does not know the password (it is not who it says)");
            return;
        }
        n->delay = message.value("delay").toInt(4);
        if (n->delay < 0 || n->delay > 15)
            n->delay = 4;
        SetSessionKey(n);
        n->send_seq = n->recv_seq = 0;
        n->status = NETPLAY_READY;
        QObject::connect(n->socket, &QWebSocket::binaryMessageReceived, [n](const QByteArray &data) { HandleBinary(n, data); });
        StartPinging(n);
    } else if (type == "deny") {
        Close(n, "refused: " + message.value("why").toString().toStdString());
    }
}

void Close(NetPlay *n, const std::string &reason, bool tell, const char *bye) {
    if (n->status == NETPLAY_CLOSED)
        return;
    n->status = NETPLAY_CLOSED;
    n->reason = reason;
    Detach(n);
    if (n->socket != nullptr) {
        if (tell && n->session_key.size() > 0)
            Send(n, Msg_Bye, QByteArray(bye != nullptr ? bye : ""));
        n->socket->close();
    }
    if (n->pending != nullptr)
        n->pending->close();
    if (n->server != nullptr)
        n->server->close();
}

// One announcement: the WebSocket port and the session code's tag, to the local network's broadcast addresses (and this machine's own loopback: two copies of the game on one machine find each other too)
void AnnounceNow(NetPlay *n) {
    if (n->announce_socket == nullptr || n->server == nullptr || n->status != NETPLAY_LISTENING) {
        if (n->announce_timer != nullptr)
            n->announce_timer->stop();
        return;
    }
    const QByteArray message = QJsonDocument(QJsonObject{ { "p", "paradox-netplay" }, { "tag", QString::fromLatin1(SessionTag(n->game, n->password)) }, { "port", (int)n->server->serverPort() } }).toJson(QJsonDocument::Compact);
    n->announce_socket->writeDatagram(message, QHostAddress(QHostAddress::Broadcast), n->discovery_port);
    n->announce_socket->writeDatagram(message, QHostAddress(QHostAddress::LocalHost), n->discovery_port);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull())
                n->announce_socket->writeDatagram(message, entry.broadcast(), n->discovery_port);
        }
    }
}

void Reset(NetPlay *n) {
    Detach(n);
    delete n->announce_timer;
    n->announce_timer = nullptr;
    delete n->announce_socket;
    n->announce_socket = nullptr;
    delete n->search_socket;
    n->search_socket = nullptr;
    n->found = false;
    n->search_tag.clear();
    if (n->socket != nullptr) { // (a host's socket belongs to its server: it goes first, and the server must not find it gone)
        n->socket->disconnect();
        n->socket->abort();
        delete n->socket.data();
    }
    n->socket = nullptr;
    if (n->pending != nullptr) {
        n->pending->disconnect();
        n->pending->abort();
        delete n->pending.data();
    }
    n->pending = nullptr;
    if (n->server != nullptr) {
        n->server->close();
        delete n->server;
        n->server = nullptr;
    }
    delete n->auth_timer;
    n->auth_timer = nullptr;
    delete n->ping_timer;
    n->ping_timer = nullptr;
    n->status = NETPLAY_IDLE;
    n->reason.clear();
    n->nonce.clear();
    n->client_nonce.clear();
    n->key.clear();
    n->session_key.clear();
    n->send_seq = n->recv_seq = 0;
    n->frame = n->sent_through = n->hash_reported = 0;
    n->local_inputs.clear();
    n->remote_inputs.clear();
    n->local_hashes.clear();
    n->remote_hashes.clear();
    n->desync = false;
    n->start_data.clear();
    n->failures = 0;
    n->is_blocked = false;
    n->ping_ms = -1;
}

}

NetPlay *NetPlay_Create(void) {
    return new NetPlay();
}

void NetPlay_Destroy(NetPlay *n) {
    if (n == nullptr)
        return;
    Reset(n);
    delete n;
}

bool NetPlay_Host(NetPlay *n, const char *game, const char *bind_address, uint16_t port, const char *password, int input_delay) {
    EnsureApplication();
    Reset(n);
    n->host = true;
    n->game = game != nullptr ? game : "";
    n->password = (password != nullptr && password[0] != 0) ? password : MakePassword();
    n->delay = input_delay < 0 ? 0 : input_delay > 15 ? 15 : input_delay;
    n->server = new QWebSocketServer(QStringLiteral("paradox-netplay"), QWebSocketServer::NonSecureMode);
    n->server->setMaxPendingConnections(2);
    QObject::connect(n->server, &QWebSocketServer::newConnection, [n]() { HostNewConnection(n); });
    n->auth_timer = new QTimer();
    n->auth_timer->setSingleShot(true);
    QObject::connect(n->auth_timer, &QTimer::timeout, [n]() {
        if (n->pending != nullptr) {
            QWebSocket *late = n->pending;
            n->pending = nullptr;
            late->close();
        }
    });
    const QHostAddress address = (bind_address == nullptr || bind_address[0] == 0) ? QHostAddress(QHostAddress::Any) : QHostAddress(QString::fromUtf8(bind_address));
    if (!n->server->listen(address, port)) {
        n->reason = n->server->errorString().toStdString();
        n->status = NETPLAY_CLOSED;
        return false;
    }
    n->announce_socket = new QUdpSocket();
    n->announce_timer = new QTimer();
    QObject::connect(n->announce_timer, &QTimer::timeout, [n]() { AnnounceNow(n); });
    n->status = NETPLAY_LISTENING;
    AnnounceNow(n);
    n->announce_timer->start(500);
    return true;
}

bool NetPlay_JoinByCode(NetPlay *n, const char *game, const char *code) {
    EnsureApplication();
    const uint16_t discovery = n->discovery_port;
    Reset(n);
    n->discovery_port = discovery;
    n->host = false;
    n->game = game != nullptr ? game : "";
    n->password = code != nullptr ? code : "";
    n->search_tag = SessionTag(n->game, n->password);
    n->search_socket = new QUdpSocket();
    if (!n->search_socket->bind(QHostAddress(QHostAddress::AnyIPv4), n->discovery_port, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        n->reason = n->search_socket->errorString().toStdString();
        n->status = NETPLAY_CLOSED;
        return false;
    }
    QObject::connect(n->search_socket, &QUdpSocket::readyRead, [n]() {
        while (n->search_socket != nullptr && n->search_socket->hasPendingDatagrams()) {
            const QNetworkDatagram datagram = n->search_socket->receiveDatagram();
            const QJsonObject message = QJsonDocument::fromJson(datagram.data()).object();
            if (message.value("p").toString() != "paradox-netplay" || message.value("tag").toString().toLatin1() != n->search_tag || n->found)
                continue;
            const int port = message.value("port").toInt();
            bool ok = false;
            const quint32 v4 = datagram.senderAddress().toIPv4Address(&ok);
            if (!ok || port <= 0 || port > 65535)
                continue;
            n->found_host = QHostAddress(v4).toString();
            n->found_port = (uint16_t)port;
            n->found = true;
        }
    });
    n->status = NETPLAY_SEARCHING;
    return true;
}

void NetPlay_SetDiscoveryPort(NetPlay *n, uint16_t port) {
    n->discovery_port = port != 0 ? port : NETPLAY_DISCOVERY_PORT;
}

bool NetPlay_Join(NetPlay *n, const char *game, const char *host, uint16_t port, const char *password) {
    EnsureApplication();
    Reset(n);
    n->host = false;
    n->game = game != nullptr ? game : "";
    n->password = password != nullptr ? password : "";
    n->socket = new QWebSocket();
    n->status = NETPLAY_CONNECTING;
    QObject::connect(n->socket, &QWebSocket::textMessageReceived, [n](const QString &text) { GuestHandleText(n, text); });
    QObject::connect(n->socket, &QWebSocket::disconnected, [n]() {
        if (n->status != NETPLAY_CLOSED)
            Close(n, n->status == NETPLAY_CONNECTING ? "the host closed the connection (is the password right?)" : "the other side has gone");
    });
    QObject::connect(n->socket, &QWebSocket::errorOccurred, [n](QAbstractSocket::SocketError) {
        if (n->status != NETPLAY_CLOSED)
            Close(n, n->socket->errorString().toStdString());
    });
    n->socket->open(QUrl(QStringLiteral("ws://%1:%2/").arg(QString::fromUtf8(host != nullptr ? host : "127.0.0.1")).arg(port)));
    return true;
}

void NetPlay_Stop(NetPlay *n) {
    if (n->status == NETPLAY_READY || n->status == NETPLAY_RUNNING)
        Close(n, "ended here", true, "");
    if (n->socket != nullptr)
        n->socket->flush();
    NetPlay_Poll(n);
    const std::string reason = n->reason;
    Reset(n);
    n->reason = reason;
    n->status = NETPLAY_CLOSED;
}

void NetPlay_Poll(NetPlay *n) {
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
    if (n->status == NETPLAY_SEARCHING && n->found) { // (the host has been heard: join it, outside the socket's signal)
        const std::string game = n->game, code = n->password, host = n->found_host.toStdString();
        const uint16_t port = n->found_port, discovery = n->discovery_port;
        NetPlay_Join(n, game.c_str(), host.c_str(), port, code.c_str());
        n->discovery_port = discovery;
    }
}

NetPlayStatus NetPlay_Status(const NetPlay *n) { return n->status; }
bool NetPlay_IsHost(const NetPlay *n) { return n->host; }
int NetPlay_LocalPlayer(const NetPlay *n) { return n->host ? 0 : 1; }
const char *NetPlay_Password(const NetPlay *n) { return n->password.c_str(); }
uint16_t NetPlay_Port(const NetPlay *n) { return n->server != nullptr ? n->server->serverPort() : 0; }
const char *NetPlay_Reason(const NetPlay *n) { return n->reason.c_str(); }
int NetPlay_InputDelay(const NetPlay *n) { return n->delay; }
int NetPlay_PingMs(const NetPlay *n) { return n->ping_ms; }

bool NetPlay_StartMatch(NetPlay *n, const uint8_t *data, size_t size) {
    if (!n->host || n->status != NETPLAY_READY || size > (size_t)kMaxStartData)
        return false;
    Send(n, Msg_Start, QByteArray(reinterpret_cast<const char *>(data), (int)size));
    n->start_data.assign(data, data + size);
    n->status = NETPLAY_RUNNING;
    n->frame = 0;
    return true;
}

const uint8_t *NetPlay_StartData(const NetPlay *n, size_t *size) {
    if (size != nullptr)
        *size = n->start_data.size();
    return n->start_data.data();
}

bool NetPlay_Frame(NetPlay *n, uint8_t local_pad, uint8_t *pad1, uint8_t *pad2) {
    if (n->status != NETPLAY_RUNNING)
        return false;
    const uint32_t delay = (uint32_t)n->delay;
    // this frame's pad is for the frame `delay` on (once: asking again for a frame that waits does not give another)
    const uint32_t target = n->frame + delay;
    if (n->sent_through <= target) {
        if (target >= delay) { // (the first `delay` frames are idle on both sides, and need nothing sent)
            n->local_inputs[target] = local_pad;
            QByteArray payload = U32(target);
            payload.append((char)local_pad);
            Send(n, Msg_Input, payload);
        }
        n->sent_through = target + 1;
    }
    uint8_t mine = 0, theirs = 0;
    if (n->frame >= delay) {
        auto local = n->local_inputs.find(n->frame);
        auto remote = n->remote_inputs.find(n->frame);
        if (local == n->local_inputs.end() || remote == n->remote_inputs.end())
            return false;
        mine = local->second;
        theirs = remote->second;
        n->local_inputs.erase(local);
        n->remote_inputs.erase(remote);
    }
    *pad1 = n->host ? mine : theirs;
    *pad2 = n->host ? theirs : mine;
    n->frame++;
    return true;
}

uint32_t NetPlay_FrameNumber(const NetPlay *n) { return n->frame; }

bool NetPlay_HashDue(const NetPlay *n) {
    return n->status == NETPLAY_RUNNING && n->frame > 0 && n->frame % kHashEvery == 0 && n->hash_reported != n->frame;
}

void NetPlay_ReportHash(NetPlay *n, uint32_t hash) {
    if (!NetPlay_HashDue(n))
        return;
    n->hash_reported = n->frame;
    n->local_hashes[n->frame] = hash;
    QByteArray payload = U32(n->frame);
    payload.append(U32(hash));
    Send(n, Msg_Hash, payload);
    CheckHashes(n, n->frame);
}

bool NetPlay_Desynced(const NetPlay *n, uint32_t *frame) {
    if (frame != nullptr)
        *frame = n->desync_frame;
    return n->desync;
}
