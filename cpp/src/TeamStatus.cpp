#include "TeamStatus.h"

#include "MemoryFormat.h"
#include "TimelineStatus.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace {

qint64 msValue(const QJsonValue &value, qint64 fallback) {
    if (!value.isDouble()) {
        return fallback;
    }
    return qint64(value.toDouble());
}

double doubleValue(const QJsonValue &value, double fallback) {
    if (!value.isDouble()) {
        return fallback;
    }
    return value.toDouble();
}

bool parseMachine(const QJsonObject &object, TeamMachine *machine,
                  QString *error) {
    machine->id = object.value(QStringLiteral("id")).toString().trimmed();
    if (machine->id.isEmpty()) {
        *error = QStringLiteral("a machine row has no id");
        return false;
    }
    machine->name = object.value(QStringLiteral("name")).toString().trimmed();
    if (machine->name.isEmpty()) {
        machine->name = machine->id;
    }
    machine->platform = object.value(QStringLiteral("platform")).toString();
    machine->version = object.value(QStringLiteral("version")).toString();
    machine->state = object.value(QStringLiteral("state")).toString().trimmed();
    if (machine->state.isEmpty()) {
        *error = QStringLiteral("machine %1 has no state").arg(machine->id);
        return false;
    }
    machine->firstSeenMs =
        msValue(object.value(QStringLiteral("firstSeenMs")), -1);
    machine->lastSeenMs =
        msValue(object.value(QStringLiteral("lastSeenMs")), -1);
    machine->intervalMs = int(msValue(object.value(QStringLiteral("intervalMs")), 0));
    machine->duplicate =
        object.value(QStringLiteral("duplicate")).toBool(false);
    machine->duplicateName =
        object.value(QStringLiteral("duplicateName")).toString();

    const QJsonValue latest = object.value(QStringLiteral("latest"));
    if (latest.isObject()) {
        const QJsonObject sample = latest.toObject();
        machine->latest.present = true;
        machine->latest.tsMs = msValue(sample.value(QStringLiteral("tsMs")), -1);
        machine->latest.cpu =
            doubleValue(sample.value(QStringLiteral("cpu")), -1.0);
        machine->latest.memAvailableKb =
            msValue(sample.value(QStringLiteral("memAvailableKb")), -1);
        machine->latest.load1 =
            doubleValue(sample.value(QStringLiteral("load1")), -1.0);
    }
    return true;
}

bool schemaOk(const QJsonObject &root, QString *error) {
    if (!root.contains(QStringLiteral("schema"))) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "relay answer has no schema - it is not a Crowsnest relay");
        }
        return false;
    }
    const int schema = int(msValue(root.value(QStringLiteral("schema")), -1));
    if (schema == kTeamSchemaVersion) {
        return true;
    }
    if (error != nullptr) {
        *error = schema > kTeamSchemaVersion
            ? QStringLiteral("relay speaks schema %1, this client reads %2 - "
                             "update Crowsnest")
                  .arg(schema)
                  .arg(kTeamSchemaVersion)
            : QStringLiteral("relay speaks schema %1, this client reads %2 - "
                             "update the relay")
                  .arg(schema)
                  .arg(kTeamSchemaVersion);
    }
    return false;
}

} // namespace

bool teamParseFleetJson(const QByteArray &body, TeamStatus *status,
                        QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer is not JSON: %1")
                         .arg(parseError.errorString());
        }
        return false;
    }
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer is not a fleet document");
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (!schemaOk(root, error)) {
        return false;
    }
    const QJsonValue machines = root.value(QStringLiteral("machines"));
    if (!machines.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer has no machines array");
        }
        return false;
    }

    TeamStatus parsed;
    parsed.answered = true;
    parsed.asOfMs = msValue(root.value(QStringLiteral("asOfMs")), -1);
    parsed.httpStatus = 200;
    const QJsonObject counts = root.value(QStringLiteral("counts")).toObject();
    parsed.counts.total = int(msValue(counts.value(QStringLiteral("total")), 0));
    parsed.counts.reporting =
        int(msValue(counts.value(QStringLiteral("reporting")), 0));
    parsed.counts.silent = int(msValue(counts.value(QStringLiteral("silent")), 0));
    parsed.counts.suspect =
        int(msValue(counts.value(QStringLiteral("suspect")), 0));
    parsed.counts.pruned = int(msValue(counts.value(QStringLiteral("pruned")), 0));

    const QJsonArray rows = machines.toArray();
    parsed.machines.reserve(rows.size());
    for (const QJsonValue &row : rows) {
        if (!row.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("a machine row is not an object");
            }
            return false;
        }
        TeamMachine machine;
        if (!parseMachine(row.toObject(), &machine, error)) {
            return false;
        }
        parsed.machines.append(machine);
    }

    // The caller's link fields (enabled, token, self id, poll cadence) stay;
    // everything the relay said is replaced.
    if (status != nullptr) {
        status->answered = true;
        status->polling = false;
        status->httpStatus = 200;
        status->error.clear();
        status->asOfMs = parsed.asOfMs;
        status->counts = parsed.counts;
        status->machines = parsed.machines;
    }
    return true;
}

int teamSelfIndex(const TeamStatus &status) {
    if (status.selfId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < status.machines.size(); ++i) {
        if (status.machines.at(i).id == status.selfId) {
            return i;
        }
    }
    return -1;
}

QList<TeamMachine> teamWithoutSelf(const TeamStatus &status) {
    QList<TeamMachine> out;
    const int self = teamSelfIndex(status);
    out.reserve(status.machines.size());
    for (int i = 0; i < status.machines.size(); ++i) {
        if (i != self) {
            out.append(status.machines.at(i));
        }
    }
    return out;
}

TeamCounts teamOwnCounts(const TeamStatus &status) {
    const QList<TeamMachine> rows = teamWithoutSelf(status);
    TeamCounts counts;
    counts.total = rows.size();
    for (const TeamMachine &machine : rows) {
        if (teamMachineSilent(status, machine)) {
            ++counts.silent;
        } else if (machine.state == QLatin1String("suspect")) {
            ++counts.suspect;
        } else if (machine.state == QLatin1String("pruned")) {
            ++counts.pruned;
        } else if (machine.state == QLatin1String("reporting")) {
            ++counts.reporting;
        }
    }
    return counts;
}

qint64 teamSilentThresholdMs(const TeamMachine &machine) {
    const qint64 interval = machine.intervalMs > 0 ? machine.intervalMs
                                                   : kTeamDefaultPollMs;
    return qMax<qint64>(3 * interval, kTeamSilentFloorMs);
}

bool teamMachineSilent(const TeamStatus &status, const TeamMachine &machine) {
    if (machine.state == QLatin1String("silent")
        || machine.state == QLatin1String("pruned")) {
        return true;
    }
    if (machine.lastSeenMs < 0 || status.asOfMs < 0) {
        return false;
    }
    return status.asOfMs - machine.lastSeenMs > teamSilentThresholdMs(machine);
}

bool teamMachineValuesTrustworthy(const TeamStatus &status,
                                  const TeamMachine &machine) {
    if (machine.state != QLatin1String("reporting")) {
        return false;
    }
    if (teamMachineSilent(status, machine)) {
        return false;
    }
    return machine.latest.present;
}

QString teamStateText(const TeamStatus &status, const TeamMachine &machine) {
    if (teamMachineSilent(status, machine)) {
        return QStringLiteral("silent");
    }
    return machine.state;
}

QString teamStateCell(const TeamStatus &status, const TeamMachine &machine) {
    const bool silent = teamMachineSilent(status, machine);
    const QString word = teamStateText(status, machine);
    if (machine.lastSeenMs < 0) {
        return silent ? word
                      : (machine.state == QLatin1String("registered")
                             ? QStringLiteral("registered - never seen")
                             : word);
    }
    const qint64 age = status.asOfMs >= 0
        ? qMax<qint64>(0, status.asOfMs - machine.lastSeenMs)
        : -1;
    if (silent || machine.state == QLatin1String("suspect")) {
        return age >= 0 ? QStringLiteral("%1 %2").arg(word, timelineAgeText(age))
                        : word;
    }
    return word;
}

QString teamCellCpu(const TeamStatus &status, const TeamMachine &machine) {
    if (!teamMachineValuesTrustworthy(status, machine)
        || machine.latest.cpu < 0.0) {
        return QStringLiteral("--");
    }
    return QString::number(machine.latest.cpu, 'f', 1);
}

QString teamCellMem(const TeamStatus &status, const TeamMachine &machine) {
    if (!teamMachineValuesTrustworthy(status, machine)
        || machine.latest.memAvailableKb < 0) {
        return QStringLiteral("--");
    }
    return formatBytesCompact(
        static_cast<unsigned long long>(machine.latest.memAvailableKb) * 1024ULL);
}

QString teamCellLoad(const TeamStatus &status, const TeamMachine &machine) {
    if (!teamMachineValuesTrustworthy(status, machine)
        || machine.latest.load1 < 0.0) {
        return QStringLiteral("--");
    }
    return QString::number(machine.latest.load1, 'f', 2);
}

QString teamCellLastSeen(const TeamStatus &status, const TeamMachine &machine) {
    if (machine.lastSeenMs < 0 || status.asOfMs < 0) {
        return QStringLiteral("--");
    }
    return timelineAgeText(qMax<qint64>(0, status.asOfMs - machine.lastSeenMs));
}

QString teamCellSource(const TeamStatus &status, const TeamMachine &machine) {
    if (teamSelfIndex(status) >= 0
        && machine.id == status.selfId) {
        return QStringLiteral("relay (you)");
    }
    return QStringLiteral("relay");
}

QString teamRowName(const TeamStatus &status, const TeamMachine &machine) {
    const QString name = machine.name.isEmpty() ? machine.id : machine.name;
    if (!status.selfId.isEmpty() && machine.id == status.selfId) {
        return QStringLiteral("* %1").arg(name);
    }
    return name;
}

QString teamSummaryText(const TeamStatus &status, qint64 nowMs) {
    if (!status.configured) {
        return teamLinkText(status, nowMs);
    }
    if (status.answered) {
        const TeamCounts counts = teamOwnCounts(status);
        const QString readAge = status.fetchedAtMs >= 0
            ? timelineAgeText(qMax<qint64>(0, nowMs - status.fetchedAtMs))
            : QStringLiteral("--");
        if (!status.error.isEmpty()) {
            return QStringLiteral("relay unreachable: %1 - showing the answer "
                                  "from %2 ago")
                .arg(status.error, readAge);
        }
        if (counts.total == 0) {
            return teamNoMachinesText();
        }
        QStringList parts;
        parts << QStringLiteral("%1 machine(s)").arg(counts.total);
        parts << QStringLiteral("%1 reporting").arg(counts.reporting);
        if (counts.suspect > 0) {
            parts << QStringLiteral("%1 suspect").arg(counts.suspect);
        }
        if (counts.silent > 0) {
            parts << QStringLiteral("%1 silent").arg(counts.silent);
        }
        if (counts.pruned > 0) {
            parts << QStringLiteral("%1 pruned").arg(counts.pruned);
        }
        return QStringLiteral("%1 - relay answered %2 ago")
            .arg(parts.join(QStringLiteral(", ")), readAge);
    }
    if (status.httpStatus >= 400) {
        return teamRefusalText(status.httpStatus, status.error);
    }
    if (!status.error.isEmpty() && status.httpStatus == 200) {
        return QStringLiteral(
                   "the relay answered, but not with a fleet this client can "
                   "read: %1")
            .arg(status.error);
    }
    if (!status.error.isEmpty()) {
        return teamUnreachableText(status, nowMs);
    }
    return teamLinkText(status, nowMs);
}

QString teamNoteText(const TeamStatus &status) {
    if (!status.enabled) {
        return QStringLiteral(
            "reading: off ([remote] enabled=false) - nothing is read from a "
            "relay; the switch is the whole upgrade, and the local console "
            "never depends on it");
    }
    if (!status.tokenConfigured) {
        return QStringLiteral(
            "reading: no [remote] token= - the relay answers 401 without one, "
            "and a machine's ingest token is the credential this page needs");
    }
    const QString endpoint = status.url.isEmpty()
        ? QStringLiteral("/api/v1/machines")
        : status.url + QStringLiteral("/api/v1/machines");
    return QStringLiteral(
        "reading: %1 every %2s with a %3ms budget - the answer carries machine "
        "state and the newest sample, never your process list, command lines "
        "or environment")
        .arg(endpoint)
        .arg(qMax(1, status.pollMs / 1000))
        .arg(status.timeoutMs);
}

QString teamLinkText(const TeamStatus &status, qint64 nowMs) {
    Q_UNUSED(nowMs);
    if (!status.enabled) {
        return QStringLiteral(
            "not linked - set [remote] enabled=true, [remote] url= and "
            "[remote] token= to show the relay's fleet here; this page is the "
            "same read the browser dashboard makes");
    }
    if (status.url.isEmpty()) {
        return QStringLiteral(
            "not linked - [remote] url= is empty or not http/https; the relay "
            "address is required before [remote] token= can prove anything");
    }
    if (!status.tokenConfigured) {
        return QStringLiteral(
            "linked to %1 but no [remote] token= - set a machine's ingest "
            "token to read the fleet (the relay answers 401 without one)")
            .arg(status.url);
    }
    return QStringLiteral("waiting for the first answer from %1").arg(status.url);
}

QString teamUnreachableText(const TeamStatus &status, qint64 nowMs) {
    QString reason = status.error.trimmed();
    if (reason.isEmpty()) {
        reason = QStringLiteral("no answer");
    }
    if (status.answered && status.fetchedAtMs >= 0) {
        return QStringLiteral("relay unreachable: %1 - the table is the answer "
                              "from %2 ago")
            .arg(reason,
                 timelineAgeText(qMax<qint64>(0, nowMs - status.fetchedAtMs)));
    }
    return QStringLiteral("relay unreachable: %1 - nothing has answered yet")
        .arg(reason);
}

QString teamNoMachinesText() {
    return QStringLiteral(
        "the relay answered and says nothing yet - no machine has reported to "
        "this workspace; one appears here the first time it reports");
}

QString teamRefusalText(int httpStatus, const QString &detail) {
    switch (httpStatus) {
    case 401:
        return QStringLiteral("relay refused the token (401): %1")
            .arg(detail.isEmpty() ? QStringLiteral("not recognized")
                                  : detail);
    case 403:
        return QStringLiteral("relay refused this token (403): %1")
            .arg(detail.isEmpty() ? QStringLiteral("forbidden") : detail);
    case 404:
        // A relay without the read route, or a path typo: the token never got
        // as far as being checked, which is the one thing this line says.
        return QStringLiteral("relay does not recognise this token (404)");
    default:
        break;
    }
    return QStringLiteral("relay answered HTTP %1%2")
        .arg(httpStatus)
        .arg(detail.isEmpty() ? QString()
                              : QStringLiteral(": ") + detail);
}

QString teamErrorDetail(const QByteArray &body) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return QString();
    }
    const QJsonObject object = document.object();
    QString detail = object.value(QStringLiteral("detail")).toString().trimmed();
    if (detail.isEmpty()) {
        detail = object.value(QStringLiteral("error")).toString().trimmed();
    }
    return detail;
}

QString teamMachineDetailText(const TeamStatus &status,
                              const TeamMachine &machine) {
    QStringList parts;
    parts << QStringLiteral("%1 (%2)").arg(machine.name, machine.id);
    parts << QStringLiteral("state %1").arg(machine.state);
    if (!machine.platform.isEmpty() || !machine.version.isEmpty()) {
        parts << QStringLiteral("%1 %2").arg(machine.platform, machine.version)
                     .trimmed();
    }
    const qint64 interval = machine.intervalMs > 0 ? machine.intervalMs
                                                   : kTeamDefaultPollMs;
    parts << QStringLiteral("reports every %1s").arg(interval / 1000.0, 0, 'g', 3);
    parts << QStringLiteral("silent after %1")
                 .arg(timelineAgeText(teamSilentThresholdMs(machine)));
    parts << QStringLiteral("last seen %1").arg(teamCellLastSeen(status, machine));
    if (machine.latest.present && status.asOfMs >= 0 && machine.latest.tsMs >= 0) {
        parts << QStringLiteral("newest sample %1 old")
                     .arg(timelineAgeText(qMax<qint64>(
                         0, status.asOfMs - machine.latest.tsMs)));
    }
    if (machine.duplicate) {
        parts << QStringLiteral("duplicate of %1")
                     .arg(machine.duplicateName.isEmpty()
                              ? QStringLiteral("another machine")
                              : machine.duplicateName);
    }
    if (!teamMachineValuesTrustworthy(status, machine)) {
        parts << QStringLiteral("values withheld: a reading past the silence "
                                "threshold is a guess");
    }
    return parts.join(QStringLiteral(" - "));
}

// ---------------------------------------------------------------------------
// M152: the incident queue and the action gates.
// ---------------------------------------------------------------------------

bool teamParseIncidentsJson(const QByteArray &body, TeamStatus *status,
                            QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer is not JSON: %1")
                         .arg(parseError.errorString());
        }
        return false;
    }
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer is not an incident document");
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (!schemaOk(root, error)) {
        return false;
    }
    const QJsonValue rows = root.value(QStringLiteral("incidents"));
    if (!rows.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer has no incidents array");
        }
        return false;
    }

    QList<TeamIncident> parsed;
    const QJsonArray array = rows.toArray();
    parsed.reserve(array.size());
    for (const QJsonValue &row : array) {
        if (!row.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("an incident row is not an object");
            }
            return false;
        }
        const QJsonObject object = row.toObject();
        TeamIncident incident;
        incident.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (incident.id.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("an incident row has no id");
            }
            return false;
        }
        incident.groupKey = object.value(QStringLiteral("groupKey")).toString();
        incident.ruleId = object.value(QStringLiteral("ruleId")).toString();
        incident.machineId =
            object.value(QStringLiteral("machineId")).toString();
        incident.severity =
            object.value(QStringLiteral("severity")).toString();
        incident.title = object.value(QStringLiteral("title")).toString();
        incident.state = object.value(QStringLiteral("state")).toString();
        incident.owner = object.value(QStringLiteral("owner")).toString();
        incident.acknowledgedBy =
            object.value(QStringLiteral("acknowledgedBy")).toString();
        incident.closedBy =
            object.value(QStringLiteral("closedBy")).toString();
        incident.openedAtMs =
            msValue(object.value(QStringLiteral("openedAtMs")), -1);
        incident.acknowledgedAtMs =
            msValue(object.value(QStringLiteral("acknowledgedAtMs")), -1);
        incident.resolvedAtMs =
            msValue(object.value(QStringLiteral("resolvedAtMs")), -1);
        incident.refires =
            int(msValue(object.value(QStringLiteral("refires")), 0));
        incident.flapping =
            object.value(QStringLiteral("flapping")).toBool(false);
        for (const QJsonValue &machine :
             object.value(QStringLiteral("machines")).toArray()) {
            if (machine.isString()) {
                incident.machines.append(machine.toString());
            }
        }
        parsed.append(incident);
    }
    if (status != nullptr) {
        status->incidentsAnswered = true;
        status->incidents = parsed;
    }
    return true;
}

QList<TeamIncident> teamActiveIncidents(const TeamStatus &status) {
    QList<TeamIncident> out;
    for (const TeamIncident &incident : status.incidents) {
        if (incident.state != QLatin1String("resolved")) {
            out.append(incident);
        }
    }
    return out;
}

QString teamIncidentStateCell(const TeamStatus &status,
                              const TeamIncident &incident) {
    if (incident.state == QLatin1String("resolved")) {
        if (incident.closedBy.isEmpty()) {
            return QStringLiteral("resolved");
        }
        return incident.closedBy == status.actor
            ? QStringLiteral("closed by you")
            : QStringLiteral("closed by %1").arg(incident.closedBy);
    }
    if (incident.state == QLatin1String("acknowledged")) {
        if (incident.acknowledgedBy.isEmpty()) {
            return QStringLiteral("acknowledged");
        }
        return incident.acknowledgedBy == status.actor
            ? QStringLiteral("ack by you")
            : QStringLiteral("ack by %1").arg(incident.acknowledgedBy);
    }
    return QStringLiteral("open");
}

QString teamIncidentMachineName(const TeamStatus &status,
                                const QString &machineId) {
    if (machineId.isEmpty()) {
        return QStringLiteral("--");
    }
    for (const TeamMachine &machine : status.machines) {
        if (machine.id == machineId) {
            return machine.name.isEmpty() ? machine.id : machine.name;
        }
    }
    return machineId;
}

QString teamIncidentSummaryText(const TeamStatus &status) {
    if (!status.incidentsAnswered) {
        return QStringLiteral("the relay's incident queue has not answered yet");
    }
    int open = 0;
    int acked = 0;
    for (const TeamIncident &incident : teamActiveIncidents(status)) {
        if (incident.state == QLatin1String("acknowledged")) {
            ++acked;
        } else {
            ++open;
        }
    }
    if (open == 0 && acked == 0) {
        return QStringLiteral("no open incidents on the relay");
    }
    QStringList parts;
    parts << QStringLiteral("%1 open").arg(open);
    parts << QStringLiteral("%1 acknowledged").arg(acked);
    return QStringLiteral("%1 - newest first").arg(parts.join(QStringLiteral(", ")));
}

QString teamIncidentDetailText(const TeamStatus &status,
                               const TeamIncident &incident) {
    QStringList parts;
    parts << QStringLiteral("%1 [%2]").arg(incident.title, incident.severity);
    parts << QStringLiteral("machine %1")
                 .arg(teamIncidentMachineName(status, incident.machineId));
    parts << QStringLiteral("rule %1").arg(incident.ruleId);
    parts << teamIncidentStateCell(status, incident);
    if (incident.openedAtMs > 0) {
        parts << QStringLiteral("opened %1 ago")
                     .arg(timelineAgeText(
                         qMax<qint64>(0, status.asOfMs - incident.openedAtMs)));
    }
    if (incident.refires > 0) {
        parts << QStringLiteral("%1 re-fire(s)").arg(incident.refires);
    }
    if (incident.flapping) {
        parts << QStringLiteral("[FLAPPING]");
    }
    if (incident.machines.size() > 1) {
        parts << QStringLiteral("folded over %1 machine(s)")
                     .arg(incident.machines.size());
    }
    if (!incident.owner.isEmpty()
        && incident.owner != incident.acknowledgedBy) {
        parts << QStringLiteral("owner %1").arg(incident.owner);
    }
    return parts.join(QStringLiteral(" - "));
}

QString teamActionsGateText(const TeamStatus &status) {
    // The switches come first: "token is read-only" is not the answer to a
    // question asked with the feature turned off.
    if (!status.enabled) {
        return QStringLiteral(
            "actions need [remote] enabled=true - this page reads nothing and "
            "writes nothing while the link is off");
    }
    if (!status.actions) {
        return QStringLiteral(
            "actions are off - set [remote] actions=true to acknowledge, close "
            "or silence from this page");
    }
    if (!status.actionsBlocked.isEmpty()) {
        return QStringLiteral("actions disabled: %1").arg(status.actionsBlocked);
    }
    return QStringLiteral(
        "actions on - acknowledge, close and silence post with the [remote] "
        "token; the next read replaces what this page showed");
}

QByteArray teamSilenceBody(const TeamIncident &incident, const QString &actor,
                           int minutes, qint64 nowMs) {
    QString matcher;
    QString scope;
    if (!incident.machineId.isEmpty()) {
        matcher = incident.machineId;
        scope = QStringLiteral("machine");
    } else if (!incident.ruleId.isEmpty()) {
        matcher = incident.ruleId;
        scope = QStringLiteral("rule");
    } else {
        // A silence with no target would mute the whole workspace; refusing is
        // the only honest answer.
        return QByteArray();
    }
    const qint64 endsAt = nowMs + qint64(qMax(1, minutes)) * 60000;
    QJsonObject body;
    body.insert(QStringLiteral("matcher"), matcher);
    body.insert(QStringLiteral("scope"), scope);
    body.insert(QStringLiteral("startsAtMs"), double(nowMs));
    body.insert(QStringLiteral("endsAtMs"), double(endsAt));
    body.insert(QStringLiteral("createdBy"), actor);
    body.insert(QStringLiteral("comment"),
                QStringLiteral("muted from the HUD: %1").arg(incident.title));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}
