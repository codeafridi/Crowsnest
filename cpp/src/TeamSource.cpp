#include "TeamSource.h"

#include "FleetSource.h"
#include "RemoteSource.h"
#include "RemoteStatus.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kTeamMinPollMs = 5000;

TeamStatus linkStatus(const TeamOptions &options) {
    TeamStatus status;
    status.enabled = options.enabled;
    status.actions = options.actions;
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

    // The queue is a second read on the same credential. Its failure is its
    // own sentence: a fleet answer is still worth showing when incidents are
    // unreachable (or refused by an older relay).
    const TeamFetchResult queue =
        teamHttpGet(options, QStringLiteral("/api/v1/incidents?state=open&limit=100"));
    if (!queue.ok) {
        status.incidentsError = queue.error;
    } else {
        QString queueFailure;
        if (!teamParseIncidentsJson(queue.body, &status, &queueFailure)) {
            status.incidentsError = queueFailure;
        }
    }
    // The checks are a third read on the same credential. An older relay
    // answers 404 here and the page says so; the fleet and incident rows stay.
    const TeamFetchResult checks =
        teamHttpGet(options, QStringLiteral("/api/v1/checks"));
    if (checks.httpStatus == 404) {
        status.checksError = QStringLiteral(
            "the relay has no checks endpoint (404) - update the relay to read "
            "its probes here");
    } else if (!checks.ok) {
        status.checksError = checks.error;
    } else {
        QString checkFailure;
        if (!teamParseChecksJson(checks.body, &status, &checkFailure)) {
            status.checksError = checkFailure;
        }
    }
    return status;
}

namespace {

// The gate every write shares: link, actions switch, token, address. A refusal
// happens before any socket, which is what the tests observe.
bool teamActionPreflight(const TeamOptions &options, TeamActionOutcome &outcome,
                         QString *baseOut) {
    if (!options.enabled) {
        outcome.detail = QStringLiteral(
            "[remote] enabled=false - nothing is written while the link is off");
        return false;
    }
    if (!options.actions) {
        outcome.detail = QStringLiteral(
            "actions are off - set [remote] actions=true to acknowledge, close "
            "or silence from the desktop");
        return false;
    }
    if (options.token.isEmpty()) {
        outcome.detail = QStringLiteral("no [remote] token= - the relay would "
                                        "answer 401");
        return false;
    }
    QString error;
    const QString base = remoteNormalizeUrl(options.baseUrl, &error);
    if (base.isEmpty()) {
        outcome.detail = error.isEmpty() ? QStringLiteral("no relay address")
                                         : error;
        return false;
    }
    if (baseOut != nullptr) {
        *baseOut = base;
    }
    return true;
}

} // namespace

TeamActionOutcome teamPerformLabelSilence(const TeamOptions &options,
                                          const QString &matcher, int minutes,
                                          qint64 nowMs) {
    TeamActionOutcome outcome;
    QString base;
    if (!teamActionPreflight(options, outcome, &base)) {
        return outcome;
    }
    const QByteArray body = teamLabelSilenceBody(
        matcher, options.actor, minutes,
        nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch());
    if (body.isEmpty()) {
        outcome.detail = QStringLiteral(
            "the label silence needs one key=value pair - the relay matches "
            "labels with the same rule");
        return outcome;
    }
    const RemotePostResult post = remoteHttpPost(
        base + QStringLiteral("/api/v1/silences"), options.token, body,
        options.timeoutMs, options.caFile);
    outcome.httpStatus = post.httpStatus;
    if (post.httpStatus >= 200 && post.httpStatus < 300) {
        outcome.ok = true;
        return outcome;
    }
    outcome.detail = teamErrorDetail(post.body);
    if (outcome.detail.isEmpty()) {
        outcome.detail = post.error;
    }
    if (outcome.detail.isEmpty()) {
        outcome.detail = QStringLiteral("HTTP %1").arg(post.httpStatus);
    }
    return outcome;
}

TeamActionOutcome teamPerformAction(const TeamOptions &options,
                                    TeamAction action,
                                    const TeamIncident &incident,
                                    int silenceMinutes, qint64 nowMs) {
    TeamActionOutcome outcome;
    QString base;
    if (!teamActionPreflight(options, outcome, &base)) {
        return outcome;
    }

    QString path;
    QByteArray body;
    switch (action) {
    case TeamAction::Ack: {
        QJsonObject object;
        object.insert(QStringLiteral("user"), options.actor);
        body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        path = QStringLiteral("/api/v1/incidents/%1/ack")
                   .arg(QString::fromLatin1(
                       QUrl::toPercentEncoding(incident.id)));
        break;
    }
    case TeamAction::Close: {
        QJsonObject object;
        object.insert(QStringLiteral("user"), options.actor);
        body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        path = QStringLiteral("/api/v1/incidents/%1/close")
                   .arg(QString::fromLatin1(
                       QUrl::toPercentEncoding(incident.id)));
        break;
    }
    case TeamAction::Silence:
        body = teamSilenceBody(incident, options.actor, silenceMinutes,
                               nowMs > 0 ? nowMs
                                         : QDateTime::currentMSecsSinceEpoch());
        if (body.isEmpty()) {
            outcome.detail = QStringLiteral(
                "this incident names no machine or rule - a silence with no "
                "target would mute the workspace");
            return outcome;
        }
        path = QStringLiteral("/api/v1/silences");
        break;
    case TeamAction::SilenceLabel:
        // Handled by teamPerformLabelSilence; this path takes an incident and
        // a label silence has none.
        outcome.detail = QStringLiteral("silence a label with its own verb");
        return outcome;
    }

    const RemotePostResult post =
        remoteHttpPost(base + path, options.token, body, options.timeoutMs,
                       options.caFile);
    outcome.httpStatus = post.httpStatus;
    if (post.httpStatus >= 200 && post.httpStatus < 300) {
        outcome.ok = true;
        return outcome;
    }
    outcome.detail = teamErrorDetail(post.body);
    if (outcome.detail.isEmpty()) {
        outcome.detail = post.error;
    }
    if (outcome.detail.isEmpty()) {
        outcome.detail = QStringLiteral("HTTP %1").arg(post.httpStatus);
    }
    return outcome;
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

    // The incident queue rides the same poll. A failed queue read never throws
    // away the rows already shown; it says why it could not refresh them.
    if (status.error.isEmpty()) {
        const TeamFetchResult queue = teamHttpGet(
            current, QStringLiteral("/api/v1/incidents?state=open&limit=100"));
        status.incidentsError.clear();
        if (!queue.ok) {
            status.incidentsError = queue.error;
            status.incidentsAnswered = previous.incidentsAnswered;
            status.incidents = previous.incidents;
        } else {
            QString queueFailure;
            if (!teamParseIncidentsJson(queue.body, &status, &queueFailure)) {
                status.incidentsError = queueFailure;
                status.incidentsAnswered = previous.incidentsAnswered;
                status.incidents = previous.incidents;
            }
        }
        // M177: the relay's checks. A failed read keeps the last rows and dates
        // them in the checks summary; it never blanks the table.
        const TeamFetchResult checks =
            teamHttpGet(current, QStringLiteral("/api/v1/checks"));
        status.checksError.clear();
        if (checks.httpStatus == 404) {
            status.checksError = QStringLiteral(
                "the relay has no checks endpoint (404) - update the relay to "
                "read its probes here");
            status.checksAnswered = previous.checksAnswered;
            status.checks = previous.checks;
            status.checksAsOfMs = previous.checksAsOfMs;
        } else if (!checks.ok) {
            status.checksError = checks.error;
            status.checksAnswered = previous.checksAnswered;
            status.checks = previous.checks;
            status.checksAsOfMs = previous.checksAsOfMs;
        } else {
            QString checkFailure;
            if (!teamParseChecksJson(checks.body, &status, &checkFailure)) {
                status.checksError = checkFailure;
                status.checksAnswered = previous.checksAnswered;
                status.checks = previous.checks;
                status.checksAsOfMs = previous.checksAsOfMs;
            }
        }
    } else {
        status.incidentsAnswered = previous.incidentsAnswered;
        status.incidents = previous.incidents;
        status.incidentsError = previous.incidentsError;
        status.checksAnswered = previous.checksAnswered;
        status.checks = previous.checks;
        status.checksError = previous.checksError;
        status.checksAsOfMs = previous.checksAsOfMs;
    }
    publish(status);
}

void TeamRunner::publish(const TeamStatus &status) {
    m_last = status;
    emit teamUpdated(status);
}

void TeamRunner::acknowledge(const TeamIncident &incident) {
    const TeamActionOutcome outcome =
        teamPerformAction(options(), TeamAction::Ack, incident, 0);
    emit actionFinished(TeamAction::Ack, incident.id, outcome.ok,
                        outcome.httpStatus, outcome.detail);
}

void TeamRunner::closeIncident(const TeamIncident &incident) {
    const TeamActionOutcome outcome =
        teamPerformAction(options(), TeamAction::Close, incident, 0);
    emit actionFinished(TeamAction::Close, incident.id, outcome.ok,
                        outcome.httpStatus, outcome.detail);
}

void TeamRunner::silenceLabel(const QString &matcher, int minutes) {
    const TeamActionOutcome outcome = teamPerformLabelSilence(
        options(), matcher, minutes, QDateTime::currentMSecsSinceEpoch());
    emit actionFinished(TeamAction::SilenceLabel, matcher, outcome.ok,
                        outcome.httpStatus, outcome.detail);
}

void TeamRunner::silenceIncident(const TeamIncident &incident, int minutes) {
    const TeamActionOutcome outcome =
        teamPerformAction(options(), TeamAction::Silence, incident, minutes);
    emit actionFinished(TeamAction::Silence, incident.id, outcome.ok,
                        outcome.httpStatus, outcome.detail);
}
