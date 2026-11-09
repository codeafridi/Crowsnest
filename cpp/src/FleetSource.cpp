#include "FleetSource.h"

#include "RemoteSource.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMutexLocker>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QtMath>

#include <memory>

namespace {

constexpr int kMinPollMs = 2000;

QString statusError(int status) {
    switch (status) {
    case 401:
        return QStringLiteral(
            "401 unauthorized - give it the same [metrics] token= this machine "
            "uses");
    case 403:
        return QStringLiteral("403 forbidden - that machine refuses this host");
    case 404:
        return QStringLiteral(
            "404 not found - it is not serving /api/v1/snapshot (set "
            "[fleet] share=true there)");
    default:
        break;
    }
    return QStringLiteral("HTTP %1").arg(status);
}

void fillHeader(const FleetPollOptions &options, FleetStatus *status) {
    status->enabled = options.enabled;
    status->share = options.share;
    status->shareAddress = options.shareAddress;
    status->sharePort = options.sharePort;
    status->tokenConfigured = !options.token.isEmpty();
    status->selfName = options.selfName;
    status->hostname = options.hostname;
    status->version = options.version;
    status->platform = options.platform;
    status->pollMs = qMax(1, options.pollMs);
    status->timeoutMs = qMax(1, options.timeoutMs);
}

void pollOne(FleetPeer &peer, const QByteArray &token, int timeoutMs,
             qint64 nowMs) {
    peer.lastAttemptMs = nowMs;
    if (!peer.configured) {
        peer.online = false;
        return;
    }
    const FleetFetchResult result = fleetHttpGet(peer.url, token, timeoutMs);
    peer.httpStatus = result.httpStatus;
    peer.latencyMs = result.latencyMs;
    if (!result.ok) {
        peer.online = false;
        peer.refused = result.httpStatus >= 400;
        peer.error = result.error;
        return;
    }
    FleetPeer parsed;
    QString failure;
    if (!parseFleetSnapshotJson(result.body, &parsed, &failure)) {
        peer.online = false;
        peer.refused = true;
        peer.error = failure;
        return;
    }
    const QString configuredName = peer.name;
    const bool authored = peer.nameAuthored;
    const QString address = peer.address;
    const QString url = peer.url;
    peer = parsed;
    peer.address = address;
    peer.url = url;
    peer.configured = true;
    peer.online = true;
    peer.refused = false;
    peer.everSeen = true;
    peer.httpStatus = result.httpStatus;
    peer.latencyMs = result.latencyMs;
    peer.lastAttemptMs = nowMs;
    peer.lastSeenMs = nowMs;
    if (authored) {
        peer.name = configuredName;
        peer.nameAuthored = true;
    } else if (peer.name.isEmpty()) {
        peer.name = configuredName;
    }
}

} // namespace

FleetFetchResult fleetHttpGet(const QString &url, const QByteArray &token,
                              int timeoutMs, const QString &caFile) {
    FleetFetchResult result;
    const QUrl target(url);
    if (!target.isValid() || target.host().isEmpty()) {
        result.error = QStringLiteral("not a usable URL: %1").arg(url);
        return result;
    }
    const int budget = qBound(100, timeoutMs, 30000);
    const bool tls =
        target.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive)
        == 0;
    const int defaultPort = tls ? 443 : 80;
    const int port = target.port(defaultPort);

    QTcpSocket plain;
    // Constructing a QSslSocket touches Qt's TLS backend, which is not
    // available in the QCoreApplication-free --dump path and warns there.
    // Only an https peer needs it.
    std::unique_ptr<QSslSocket> secure;
    QTcpSocket *socket = &plain;
    if (tls) {
        secure = std::make_unique<QSslSocket>();
        socket = secure.get();
        // A relay behind a private CA is trusted by naming the CA, never by
        // turning verification off (M100b's rule, shared with the reporter).
        if (!caFile.isEmpty()) {
            QString sslError;
            const QSslConfiguration config =
                remoteSslConfiguration(caFile, &sslError);
            if (!sslError.isEmpty()) {
                result.error = sslError;
                return result;
            }
            secure->setSslConfiguration(config);
        }
    }

    QString hostHeader = target.host();
    if (port != defaultPort) {
        hostHeader += QLatin1Char(':') + QString::number(port);
    }

    QByteArray targetPath = target.path().toUtf8();
    if (!target.query().isEmpty()) {
        targetPath += '?';
        targetPath += target.query(QUrl::FullyEncoded).toUtf8();
    }
    QByteArray request = QByteArrayLiteral("GET ") + targetPath
        + QByteArrayLiteral(" HTTP/1.1\r\nHost: ") + hostHeader.toUtf8()
        + QByteArrayLiteral("\r\nAccept: application/json\r\n");
    if (!token.isEmpty()) {
        request += QByteArrayLiteral("Authorization: Bearer ") + token
            + QByteArrayLiteral("\r\n");
    }
    request += QByteArrayLiteral("Connection: close\r\n\r\n");

    QElapsedTimer clock;
    clock.start();

    if (tls) {
        secure->connectToHostEncrypted(target.host(), quint16(port));
        if (!secure->waitForConnected(budget)) {
            result.latencyMs = int(clock.elapsed());
            result.error = secure->errorString();
            return result;
        }
        if (!secure->waitForEncrypted(budget)) {
            result.latencyMs = int(clock.elapsed());
            result.error = QStringLiteral("TLS handshake failed: %1")
                               .arg(secure->errorString());
            secure->abort();
            return result;
        }
    } else {
        plain.connectToHost(target.host(), quint16(port));
        if (!plain.waitForConnected(budget)) {
            result.latencyMs = int(clock.elapsed());
            result.error = plain.errorString();
            return result;
        }
    }

    socket->write(request);
    socket->flush();

    QByteArray raw;
    while (true) {
        if (socket->bytesAvailable() > 0) {
            raw += socket->readAll();
        }
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            raw += socket->readAll();
            break;
        }
        const int remaining = budget - int(clock.elapsed());
        if (remaining <= 0) {
            result.error = QStringLiteral("no answer within %1ms").arg(budget);
            socket->abort();
            break;
        }
        socket->waitForReadyRead(qMin(remaining, 200));
    }
    result.latencyMs = int(clock.elapsed());
    if (!result.error.isEmpty()) {
        return result;
    }

    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        result.error = QStringLiteral("no HTTP response");
        return result;
    }
    const QByteArray head = raw.left(headerEnd);
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> statusParts = lines.value(0).trimmed().split(' ');
    bool okStatus = false;
    const int status = statusParts.value(1).toInt(&okStatus);
    if (!okStatus) {
        result.error = QStringLiteral("malformed status line");
        return result;
    }
    result.httpStatus = status;

    QByteArray body = raw.mid(headerEnd + 4);
    bool chunked = false;
    for (const QByteArray &line : lines) {
        const QByteArray lower = line.trimmed().toLower();
        if (lower.startsWith("content-length:")) {
            bool okLength = false;
            const int length = lower.mid(15).trimmed().toInt(&okLength);
            if (okLength && length >= 0 && body.size() > length) {
                body.truncate(length);
            }
        } else if (lower.startsWith("transfer-encoding:")
                   && lower.contains("chunked")) {
            chunked = true;
        }
    }
    if (chunked) {
        body = fleetDechunk(body);
    }
    result.body = body;
    if (status != 200) {
        result.error = statusError(status);
        return result;
    }
    result.ok = true;
    return result;
}

QByteArray fleetDechunk(const QByteArray &body) {
    QByteArray out;
    int pos = 0;
    while (pos < body.size()) {
        const int lineEnd = body.indexOf("\r\n", pos);
        if (lineEnd < 0) {
            return body; // no framing we understand: hand the caller the bytes
        }
        bool ok = false;
        const int size =
            body.mid(pos, lineEnd - pos).trimmed().toInt(&ok, 16);
        if (!ok || size < 0) {
            return body;
        }
        pos = lineEnd + 2;
        if (size == 0) {
            break;
        }
        if (pos + size > body.size()) {
            return body;
        }
        out += body.mid(pos, size);
        pos += size;
        if (body.mid(pos, 2) == "\r\n") {
            pos += 2;
        }
    }
    return out;
}

FleetStatus probeFleet(const FleetPollOptions &options, qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    FleetStatus status;
    fillHeader(options, &status);
    QString failure;
    status.peers = parseFleetPeers(options.peers, &failure);
    if (!options.enabled) {
        status.note = fleetGateText(status);
        return status;
    }
    if (status.peers.isEmpty()) {
        status.note = fleetNoPeersText();
        return status;
    }
    const QByteArray token = options.token.toUtf8();
    for (FleetPeer &peer : status.peers) {
        pollOne(peer, token, options.timeoutMs, nowMs);
    }
    status.lastPollMs = nowMs;
    return status;
}

FleetRunner::FleetRunner(QObject *parent)
    : QObject(parent) {
}

FleetRunner::~FleetRunner() {
    stop();
}

void FleetRunner::setOptions(const FleetPollOptions &options) {
    bool changed = false;
    {
        QMutexLocker lock(&m_mutex);
        changed = m_options.peers != options.peers
            || m_options.token != options.token
            || m_options.pollMs != options.pollMs
            || m_options.timeoutMs != options.timeoutMs
            || m_options.enabled != options.enabled;
        m_options = options;
    }
    if (changed && m_timer != nullptr) {
        QMetaObject::invokeMethod(this, "pollNow", Qt::QueuedConnection);
    }
}

FleetPollOptions FleetRunner::options() const {
    QMutexLocker lock(&m_mutex);
    return m_options;
}

void FleetRunner::start() {
    if (m_timer == nullptr) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::CoarseTimer);
        connect(m_timer, &QTimer::timeout, this, &FleetRunner::poll);
    }
    m_timer->setInterval(qMax(kMinPollMs, options().pollMs));
    m_timer->start();
    poll();
}

void FleetRunner::stop() {
    if (m_timer != nullptr) {
        m_timer->stop();
    }
}

void FleetRunner::pollNow() {
    if (m_timer != nullptr) {
        m_timer->setInterval(qMax(kMinPollMs, options().pollMs));
    }
    poll();
}

FleetStatus FleetRunner::buildStatus(bool polling) const {
    FleetStatus status;
    fillHeader(options(), &status);
    status.peers = m_peers;
    status.lastPollMs = m_lastPollMs;
    status.polling = polling;
    if (m_peerSpec.isEmpty()) {
        status.note = status.enabled ? fleetNoPeersText() : fleetGateText(status);
    }
    return status;
}

void FleetRunner::poll() {
    FleetPollOptions options;
    {
        QMutexLocker lock(&m_mutex);
        options = m_options;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (options.peers != m_peerSpec) {
        QString failure;
        m_peers = parseFleetPeers(options.peers, &failure);
        m_peerSpec = options.peers;
    }
    if (!options.enabled || m_peers.isEmpty()) {
        m_lastPollMs = now;
        emit fleetUpdated(buildStatus(false));
        return;
    }

    emit fleetUpdated(buildStatus(true));
    const QByteArray token = options.token.toUtf8();
    for (FleetPeer &peer : m_peers) {
        pollOne(peer, token, options.timeoutMs, now);
        emit fleetUpdated(buildStatus(true));
    }
    m_lastPollMs = QDateTime::currentMSecsSinceEpoch();
    emit fleetUpdated(buildStatus(false));
}
