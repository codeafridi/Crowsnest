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
    // Labels arrive only from a relay that has the column; anything else is an
    // unlabelled machine, not an error - the page still has to render.
    machine->labels.clear();
    const QJsonObject labels = object.value(QStringLiteral("labels")).toObject();
    QStringList keys = labels.keys();
    keys.sort();
    for (const QString &key : keys) {
        const QJsonValue value = labels.value(key);
        if (value.isString() && !key.isEmpty()) {
            machine->labels.append({key, value.toString()});
        }
    }

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

QString teamLabelsText(const TeamMachine &machine) {
    if (machine.labels.isEmpty()) {
        return QStringLiteral("no labels");
    }
    QStringList parts;
    parts.reserve(machine.labels.size());
    for (const QPair<QString, QString> &label : machine.labels) {
        parts << QStringLiteral("%1=%2").arg(label.first, label.second);
    }
    return parts.join(QLatin1Char(' '));
}

QString teamLabelsSummaryText(const QList<TeamMachine> &machines) {
    if (machines.isEmpty()) {
        return QString();
    }
    QSet<QString> teams;
    QSet<QString> envs;
    int labelled = 0;
    for (const TeamMachine &machine : machines) {
        if (!machine.labels.isEmpty()) {
            ++labelled;
        }
        const QString team = teamLabelValue(machine, QStringLiteral("team"));
        if (!team.isEmpty()) {
            teams.insert(team);
        }
        const QString env = teamLabelValue(machine, QStringLiteral("env"));
        if (!env.isEmpty()) {
            envs.insert(env);
        }
    }
    if (labelled == 0) {
        return QStringLiteral("%1 machine(s), none labelled")
            .arg(machines.size());
    }
    QString text = QStringLiteral("%1 of %2 machine(s) labelled")
                       .arg(labelled)
                       .arg(machines.size());
    if (!teams.isEmpty()) {
        text += QStringLiteral(" - %1 team(s)").arg(teams.size());
    }
    if (!envs.isEmpty()) {
        text += QStringLiteral(" - %1 env(s)").arg(envs.size());
    }
    return text;
}

QStringList teamLabelFacets(const QList<TeamMachine> &machines) {
    QStringList facets;
    for (const TeamMachine &machine : machines) {
        for (const QPair<QString, QString> &label : machine.labels) {
            const QString facet =
                QStringLiteral("%1=%2").arg(label.first, label.second);
            if (!facets.contains(facet)) {
                facets.append(facet);
            }
        }
    }
    facets.sort();
    return facets;
}

QStringList teamLabelKeys(const QList<TeamMachine> &machines) {
    QStringList keys;
    for (const TeamMachine &machine : machines) {
        for (const QPair<QString, QString> &label : machine.labels) {
            if (!keys.contains(label.first)) {
                keys.append(label.first);
            }
        }
    }
    keys.sort();
    return keys;
}

QList<TeamGroup> teamGroups(const QList<TeamMachine> &machines,
                            const QString &key) {
    QList<TeamGroup> groups;
    if (key.isEmpty()) {
        return groups;
    }
    for (int i = 0; i < machines.size(); ++i) {
        QString value = teamLabelValue(machines.at(i), key);
        if (value.isEmpty()) {
            value = QStringLiteral("(no %1)").arg(key);
        }
        TeamGroup *group = nullptr;
        for (TeamGroup &seen : groups) {
            if (seen.value == value) {
                group = &seen;
                break;
            }
        }
        if (group == nullptr) {
            TeamGroup fresh;
            fresh.key = key;
            fresh.value = value;
            groups.append(fresh);
            group = &groups.last();
        }
        group->members.append(i);
        if (machines.at(i).state == QLatin1String("reporting")
            || machines.at(i).state == QLatin1String("suspect")
            || machines.at(i).state == QLatin1String("registered")) {
            ++group->online;
        }
        if (machines.at(i).state == QLatin1String("silent")
            || machines.at(i).state == QLatin1String("pruned")) {
            ++group->silent;
        }
    }
    std::sort(groups.begin(), groups.end(),
              [](const TeamGroup &a, const TeamGroup &b) {
                  // "(no key)" last: a group nobody labelled is the tail, not
                  // the first thing a reader sees.
                  const bool aNone = a.value.startsWith(QStringLiteral("(no "));
                  const bool bNone = b.value.startsWith(QStringLiteral("(no "));
                  if (aNone != bNone) {
                      return !aNone;
                  }
                  return a.value < b.value;
              });
    return groups;
}

QString teamGroupRowText(const TeamGroup &group) {
    QString text = QStringLiteral("%1 = %2 - %3 machine(s)")
                       .arg(group.key, group.value)
                       .arg(group.members.size());
    text += QStringLiteral(", %1 online").arg(group.online);
    if (group.silent > 0) {
        text += QStringLiteral(", %1 silent").arg(group.silent);
    }
    return text;
}

QString teamLabelValue(const TeamMachine &machine, const QString &key) {
    for (const QPair<QString, QString> &label : machine.labels) {
        if (label.first == key) {
            return label.second;
        }
    }
    return QString();
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
        "reading: %1, its incident queue and its checks every %2s with a %3ms "
        "budget - the answer carries machine state, the newest sample and the "
        "relay's own probes, never your process list, command lines or "
        "environment")
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
    // M161: the labels the operator wrote, and the owner among them when there
    // is one - the page's whole question is "whose box is this".
    parts << teamLabelsText(machine);
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
    // M164: whose machine the incident is about, from the fleet row the page
    // already holds - one lookup, no second request per incident.
    for (const TeamMachine &machine : status.machines) {
        if (machine.id != incident.machineId || machine.labels.isEmpty()) {
            continue;
        }
        QStringList pairs;
        for (const QPair<QString, QString> &label : machine.labels) {
            pairs << QStringLiteral("%1=%2").arg(label.first, label.second);
        }
        parts << pairs.join(QLatin1Char(' '));
        break;
    }
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

QByteArray teamLabelSilenceBody(const QString &matcher, const QString &actor,
                                int minutes, qint64 nowMs) {
    const QString pair = matcher.trimmed();
    const int equals = pair.indexOf(QLatin1Char('='));
    if (equals <= 0 || equals == pair.size() - 1) {
        return QByteArray();
    }
    // One pair, no whitespace: the relay's label matcher is `key=value` and a
    // second pair here would be silently dropped inside the value.
    if (pair.contains(QLatin1Char(' ')) || pair.contains(QLatin1Char('\t'))) {
        return QByteArray();
    }
    const qint64 endsAt = nowMs + qint64(qMax(1, minutes)) * 60000;
    QJsonObject body;
    body.insert(QStringLiteral("matcher"), pair);
    body.insert(QStringLiteral("scope"), QStringLiteral("label"));
    body.insert(QStringLiteral("startsAtMs"), double(nowMs));
    body.insert(QStringLiteral("endsAtMs"), double(endsAt));
    body.insert(QStringLiteral("createdBy"), actor);
    body.insert(QStringLiteral("comment"),
                QStringLiteral("muted from the HUD: every machine labelled %1")
                    .arg(pair));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
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

// ---------------------------------------------------------------------------
// M177: the relay's own checks.
// ---------------------------------------------------------------------------

bool teamParseChecksJson(const QByteArray &body, TeamStatus *status,
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
            *error = QStringLiteral("relay answer is not a checks document");
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (!schemaOk(root, error)) {
        return false;
    }
    const QJsonValue rows = root.value(QStringLiteral("checks"));
    if (!rows.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("relay answer has no checks array");
        }
        return false;
    }
    QList<TeamCheck> parsed;
    const QJsonArray array = rows.toArray();
    parsed.reserve(array.size());
    for (const QJsonValue &row : array) {
        if (!row.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("a check row is not an object");
            }
            return false;
        }
        const QJsonObject object = row.toObject();
        TeamCheck check;
        check.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (check.id.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("a check row has no id");
            }
            return false;
        }
        check.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (check.name.isEmpty()) {
            check.name = check.id;
        }
        check.kind = object.value(QStringLiteral("kind")).toString();
        check.target = object.value(QStringLiteral("target")).toString();
        check.state = object.value(QStringLiteral("state")).toString();
        if (check.state.isEmpty()) {
            check.state = QStringLiteral("unknown");
        }
        check.enabled = object.value(QStringLiteral("enabled")).toBool(false);
        check.heartbeatSet =
            object.value(QStringLiteral("heartbeatSet")).toBool(false);
        check.failing =
            int(msValue(object.value(QStringLiteral("failing")), 0));
        check.lastStatus =
            int(msValue(object.value(QStringLiteral("lastStatus")), 0));
        check.lastLatencyMs =
            int(msValue(object.value(QStringLiteral("lastLatencyMs")), 0));
        check.intervalS =
            int(msValue(object.value(QStringLiteral("intervalS")), 0));
        check.graceS = int(msValue(object.value(QStringLiteral("graceS")), 0));
        check.lastCheckedAtMs =
            msValue(object.value(QStringLiteral("lastCheckedAtMs")), -1);
        check.lastOkAtMs =
            msValue(object.value(QStringLiteral("lastOkAtMs")), -1);
        check.lastPushedAtMs =
            msValue(object.value(QStringLiteral("lastPushedAtMs")), -1);
        check.certExpiresAtMs =
            msValue(object.value(QStringLiteral("certExpiresAtMs")), -1);
        check.lastError =
            object.value(QStringLiteral("lastError")).toString();
        const QJsonObject labels = object.value(QStringLiteral("labels")).toObject();
        QStringList keys = labels.keys();
        keys.sort();
        for (const QString &key : keys) {
            const QJsonValue value = labels.value(key);
            if (value.isString() && !key.isEmpty()) {
                check.labels.append({key, value.toString()});
            }
        }
        parsed.append(check);
    }
    if (status != nullptr) {
        status->checksAnswered = true;
        status->checks = parsed;
        const qint64 asOf = msValue(root.value(QStringLiteral("asOfMs")), -1);
        status->checksAsOfMs = asOf > 0 ? asOf : status->asOfMs;
    }
    return true;
}

namespace {

qint64 checkClockMs(const TeamStatus &status) {
    if (status.checksAsOfMs > 0) {
        return status.checksAsOfMs;
    }
    if (status.asOfMs > 0) {
        return status.asOfMs;
    }
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

QString teamCheckStateText(const TeamCheck &check) {
    if (!check.enabled) {
        return QStringLiteral("paused");
    }
    return check.state.isEmpty() ? QStringLiteral("unknown") : check.state;
}

QString teamCheckTargetText(const TeamCheck &check) {
    if (check.push()) {
        return QStringLiteral("waits for a heartbeat");
    }
    return check.target.isEmpty() ? QStringLiteral("--") : check.target;
}

QString teamCheckLastText(const TeamStatus &status, const TeamCheck &check) {
    const qint64 at = check.push() ? check.lastPushedAtMs
                                   : check.lastCheckedAtMs;
    if (at <= 0) {
        return check.push() ? QStringLiteral("no push yet")
                            : QStringLiteral("never");
    }
    return QStringLiteral("%1 ago")
        .arg(timelineAgeText(qMax<qint64>(0, checkClockMs(status) - at)));
}

QString teamCheckLatencyText(const TeamCheck &check) {
    if (check.lastLatencyMs <= 0) {
        return QStringLiteral("--");
    }
    return QStringLiteral("%1 ms").arg(check.lastLatencyMs);
}

int teamCheckCertDays(const TeamStatus &status, const TeamCheck &check) {
    if (check.certExpiresAtMs <= 0) {
        return -1;
    }
    const double days =
        double(check.certExpiresAtMs - checkClockMs(status)) / 86400000.0;
    return int(std::ceil(days));
}

QString teamCheckCertText(const TeamStatus &status, const TeamCheck &check) {
    if (check.certExpiresAtMs <= 0) {
        return QStringLiteral("--");
    }
    const int days = teamCheckCertDays(status, check);
    if (days <= 0) {
        return QStringLiteral("expired");
    }
    return QStringLiteral("%1d").arg(days);
}

QString teamCheckSummaryText(const TeamStatus &status) {
    if (!status.configured) {
        return QStringLiteral(
            "checks: the relay link is off - set [remote] enabled=true and a "
            "token to read them");
    }
    if (!status.checksAnswered) {
        if (!status.checksError.isEmpty()) {
            return QStringLiteral("checks unavailable: %1")
                .arg(status.checksError);
        }
        return QStringLiteral("the relay's checks have not answered yet");
    }
    int up = 0;
    int down = 0;
    int other = 0;
    int certWatch = 0;
    for (const TeamCheck &check : status.checks) {
        if (!check.enabled) {
            ++other;
            continue;
        }
        if (check.state == QLatin1String("up")) {
            ++up;
        } else if (check.state == QLatin1String("down")) {
            ++down;
        } else {
            ++other;
        }
        if (check.certExpiresAtMs > 0 && teamCheckCertDays(status, check) <= 30) {
            ++certWatch;
        }
    }
    if (status.checks.isEmpty()) {
        return QStringLiteral(
            "the relay has no checks yet - create HTTP, TCP, TLS or push checks "
            "in its dashboard");
    }
    QStringList parts;
    parts << QStringLiteral("%1 check(s)").arg(status.checks.size());
    parts << QStringLiteral("%1 up").arg(up);
    if (down > 0) {
        parts << QStringLiteral("%1 down").arg(down);
    }
    if (other > 0) {
        parts << QStringLiteral("%1 paused/unknown").arg(other);
    }
    if (certWatch > 0) {
        parts << QStringLiteral("%1 certificate(s) under 30 days").arg(certWatch);
    }
    QString text = parts.join(QStringLiteral(", "));
    if (!status.checksError.isEmpty()) {
        text += QStringLiteral(" - the last refresh failed: %1")
                    .arg(status.checksError);
    }
    return text;
}

QString teamCheckDetailText(const TeamStatus &status, const TeamCheck &check) {
    QStringList parts;
    parts << QStringLiteral("%1 [%2]").arg(check.name, check.kind);
    parts << QStringLiteral("state %1").arg(teamCheckStateText(check));
    parts << QStringLiteral("target %1").arg(teamCheckTargetText(check));
    if (check.push()) {
        parts << QStringLiteral("interval %1s + %2s grace")
                     .arg(check.intervalS)
                     .arg(check.graceS);
    } else {
        parts << QStringLiteral("interval %1s").arg(check.intervalS);
        parts << QStringLiteral("latency %1").arg(teamCheckLatencyText(check));
        if (check.lastStatus > 0) {
            parts << QStringLiteral("status %1").arg(check.lastStatus);
        }
    }
    parts << QStringLiteral("last %1").arg(teamCheckLastText(status, check));
    if (check.certExpiresAtMs > 0) {
        parts << QStringLiteral("certificate %1")
                     .arg(teamCheckCertText(status, check));
    }
    if (check.push() && check.heartbeatSet) {
        parts << QStringLiteral("heartbeat link set");
    }
    if (!check.labels.isEmpty()) {
        QStringList pairs;
        for (const QPair<QString, QString> &label : check.labels) {
            pairs << QStringLiteral("%1=%2").arg(label.first, label.second);
        }
        parts << pairs.join(QLatin1Char(' '));
    }
    if (!check.lastError.isEmpty()) {
        parts << QStringLiteral("error %1").arg(check.lastError);
    }
    return parts.join(QStringLiteral(" - "));
}
