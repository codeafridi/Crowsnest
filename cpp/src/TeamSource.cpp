#include "TeamSource.h"

#include "FleetSource.h"
#include "RemoteStatus.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QTimer>

namespace {

constexpr int kTeamMinPollMs = 5000;

TeamStatus linkStatus(const TeamOptions &options) {
    TeamStatus status;
    status.enabled = options.enabled;
    status.configured = options.enabled && !options.token.isEmpty()
        && !remoteNormalizeUrl(options.baseUrl, nullptr).isEmpty();
    status.url = remoteNormalizeUrl(options.baseUrl, nullptr);
    status.tokenConfigured = !options.token.isEmpty();
    status.selfId = options.selfId;
    status.actor = options.actor;
    status.pollMs = qMax(1, options.pollMs);
    status.timeoutMs = qMax(1, options.timeoutMs);
    return status;
}

} // namespace

TeamFetchResult teamHttpGet(const TeamOptions &options, const QString &path) {
    TeamFetchResult result;
    QString error;
    const QString base = remoteNormalizeUrl(options.baseUrl, &error);
    if (base.isEmpty()) {
        result.error = error.isEmpty() ? QStringLiteral("no relay address")
                                       : error;
        return result;
    }
    const FleetFetchResult fetch =
        fleetHttpGet(base + path, options.token.toUtf8(), options.timeoutMs,
                     options.caFile);
    result.httpStatus = fetch.httpStatus;
    result.body = fetch.body;
    if (fetch.httpStatus == 0) {
        result.error = fetch.error;
        return result;
    }
    if (!fetch.ok) {
        result.error = teamErrorDetail(fetch.body);
        if (result.error.isEmpty()) {
            result.error = fetch.error;
        }
        return result;
    }
    result.ok = true;
    return result;
}

TeamStatus teamStatusFromOptions(const TeamOptions &options) {
    return linkStatus(options);
}

TeamStatus probeTeam(const TeamOptions &options, qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    TeamStatus status = linkStatus(options);
    if (!status.enabled) {
        return status;
    }
    if (!status.configured) {
        return status;
    }

    const TeamFetchResult result =
        teamHttpGet(options, QStringLiteral("/api/v1/machines?limit=1000"));
    status.fetchedAtMs = nowMs;
    status.httpStatus = result.httpStatus;
    if (!result.ok) {
        status.error = result.error;
        return status;
    }
    QString failure;
    if (!teamParseFleetJson(result.body, &status, &failure)) {
        // The relay answered 200 with something this client cannot read (a
        // newer schema, a proxy's HTML). Keep the status code so the page can
        // distinguish that from a transport failure.
        status.error = failure;
        return status;
    }
    status.error.clear();
    return status;
}

TeamRunner::TeamRunner(QObject *parent) : QObject(parent) {}

TeamRunner::~TeamRunner() = default;

void TeamRunner::setOptions(const TeamOptions &options) {
    QMutexLocker lock(&m_mutex);
    m_options = options;
}

TeamOptions TeamRunner::options() const {
    QMutexLocker lock(&m_mutex);
    return m_options;
}

void TeamRunner::start() {
    if (m_timer != nullptr) {
        return;
    }
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &TeamRunner::poll);
    const TeamOptions current = options();
    m_timer->start(qMax(kTeamMinPollMs, current.pollMs));
    poll();
}

void TeamRunner::stop() {
    if (m_timer != nullptr) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
}

void TeamRunner::pollNow() {
    poll();
}

void TeamRunner::poll() {
    TeamOptions current;
    {
        QMutexLocker lock(&m_mutex);
        current = m_options;
    }
    TeamStatus status = linkStatus(current);
    if (!status.configured) {
        publish(status);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const TeamFetchResult result =
        teamHttpGet(current, QStringLiteral("/api/v1/machines?limit=1000"));
    status.fetchedAtMs = now;
    status.httpStatus = result.httpStatus;

    // A failed poll keeps the last answer: the table's rows are still true,
    // and the summary names the age so nobody reads them as current.
    const TeamStatus previous = m_last;
    if (!result.ok) {
        status.error = result.error;
        status.answered = previous.answered;
        status.asOfMs = previous.asOfMs;
        status.fetchedAtMs = previous.fetchedAtMs;
        status.counts = previous.counts;
        status.machines = previous.machines;
    } else {
        QString failure;
        if (!teamParseFleetJson(result.body, &status, &failure)) {
            status.error = failure;
            status.answered = previous.answered;
            status.asOfMs = previous.asOfMs;
            status.fetchedAtMs = previous.fetchedAtMs;
            status.counts = previous.counts;
            status.machines = previous.machines;
        } else {
            status.error.clear();
        }
    }
    publish(status);
}

void TeamRunner::publish(const TeamStatus &status) {
    m_last = status;
    emit teamUpdated(status);
}
