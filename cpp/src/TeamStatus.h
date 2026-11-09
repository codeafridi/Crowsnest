#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

// M151: the relay's fleet, on the Team page.
//
// The browser dashboard is the relay's own view of the workspace; this page is
// the same read from the desktop, so the two can never disagree about which
// machine is silent. Nothing here is a second source of truth: every row is the
// relay's answer, and when the relay cannot be reached the page says so with the
// age of the last answer instead of showing a friendly lie.
//
// Three words for three different causes, on purpose (netdata's
// mirrored_hosts_status keeps `reachable`/`stale`/`offline` apart for the same
// reason): "relay unreachable", "the relay has no machines yet" and "this
// machine is silent" must never collapse into one empty table.

// Bump only when a field changes meaning. A relay that answers a NEWER schema
// is refused with the fix named, never half-parsed.
constexpr int kTeamSchemaVersion = 1;

// A machine is late when it has not been seen for max(3 intervals, 90 s),
// measured on the RELAY's clock (asOfMs - lastSeenMs), so a desktop whose clock
// is wrong cannot invent a silent machine.
constexpr int kTeamSilentFloorMs = 90000;
constexpr int kTeamDefaultPollMs = 15000;

struct TeamSample {
    bool present = false;
    qint64 tsMs = -1;
    double cpu = -1.0;
    qint64 memAvailableKb = -1;
    double load1 = -1.0;
};

struct TeamMachine {
    QString id;
    QString name;
    QString platform;
    QString version;
    QString state; // registered | reporting | suspect | silent | pruned
    qint64 firstSeenMs = -1;
    qint64 lastSeenMs = -1;
    int intervalMs = 0;
    bool duplicate = false;
    QString duplicateName;
    TeamSample latest;
};

struct TeamCounts {
    int total = 0;
    int reporting = 0;
    int silent = 0;
    int suspect = 0;
    int pruned = 0;
};

// ---------------------------------------------------------------------------
// M152: the relay's incident work queue and the three verbs the HUD may send.
// PagerDuty's model is the one the relay already speaks: triggered/acknowledged/
// resolved, an acknowledgement list with who and when, and `assignments` empty
// once resolved. The HUD never invents an owner: the row prints whoever the
// relay says holds it.
// ---------------------------------------------------------------------------

enum class TeamAction { Ack, Close, Silence };

struct TeamIncident {
    QString id;
    QString groupKey;
    QString ruleId;
    QString machineId;
    QString severity; // critical | warning | info
    QString title;
    QString state; // open | acknowledged | resolved
    QString owner;
    QString acknowledgedBy;
    QString closedBy;
    qint64 openedAtMs = -1;
    qint64 acknowledgedAtMs = -1;
    qint64 resolvedAtMs = -1;
    int refires = 0;
    bool flapping = false;
    QStringList machines;
};

// The POST body for POST /api/v1/silences. Empty when the incident carries
// neither a machine nor a rule, because a silence with no target is a silence
// of everything.
QByteArray teamSilenceBody(const TeamIncident &incident, const QString &actor,
                           int minutes, qint64 nowMs);

struct TeamStatus {
    // The link, read from [remote].
    bool enabled = false;
    bool configured = false; // enabled + a parseable url + a token
    bool actions = false;    // [remote] actions
    QString actionsBlocked;  // the relay's refusal, once a write was refused
    QString url;             // normalized base, no path
    bool tokenConfigured = false;
    QString selfId;   // this machine's relay id ([remote] machine_id)
    QString actor;    // the name an ack is posted under
    int pollMs = kTeamDefaultPollMs;
    int timeoutMs = 3000;

    // The relay's last answer.
    bool answered = false; // at least one parse succeeded
    bool polling = false;
    int httpStatus = 0;
    QString error; // transport failure or refusal reason
    qint64 asOfMs = -1;     // the relay's clock at the answer
    qint64 fetchedAtMs = -1; // the local clock the answer arrived
    TeamCounts counts;      // as the relay computed them (includes self)
    QList<TeamMachine> machines;

    // The incident queue the page acknowledges from.
    bool incidentsAnswered = false;
    QString incidentsError;
    QList<TeamIncident> incidents;
};

// Parses GET /api/v1/incidents. Same schema integer as the fleet read; a newer
// answer is refused with the fix named.
bool teamParseIncidentsJson(const QByteArray &body, TeamStatus *status,
                            QString *error);

QList<TeamIncident> teamActiveIncidents(const TeamStatus &status);
QString teamIncidentStateCell(const TeamStatus &status,
                              const TeamIncident &incident);
QString teamIncidentMachineName(const TeamStatus &status,
                                const QString &machineId);
QString teamIncidentSummaryText(const TeamStatus &status);
QString teamIncidentDetailText(const TeamStatus &status,
                               const TeamIncident &incident);

// The gate the action line prints: the switch when actions are off, the
// relay's own refusal once it has answered a write with one.
QString teamActionsGateText(const TeamStatus &status);


// Parses GET /api/v1/machines. Returns false and sets error for something that
// is not a fleet document, including a newer schema.
bool teamParseFleetJson(const QByteArray &body, TeamStatus *status,
                        QString *error);

int teamSelfIndex(const TeamStatus &status);
QList<TeamMachine> teamWithoutSelf(const TeamStatus &status);
// Counts with this machine removed, so the tiles describe the fleet the person
// is watching rather than the window they are reading it in.
TeamCounts teamOwnCounts(const TeamStatus &status);

qint64 teamSilentThresholdMs(const TeamMachine &machine);
// The relay's word, and the relay's own last-seen gap: either says silent and
// the row is silent. A value from a machine past the threshold is a guess.
bool teamMachineSilent(const TeamStatus &status, const TeamMachine &machine);
bool teamMachineValuesTrustworthy(const TeamStatus &status,
                                  const TeamMachine &machine);

QString teamStateText(const TeamStatus &status, const TeamMachine &machine);
QString teamStateCell(const TeamStatus &status, const TeamMachine &machine);
QString teamCellCpu(const TeamStatus &status, const TeamMachine &machine);
QString teamCellMem(const TeamStatus &status, const TeamMachine &machine);
QString teamCellLoad(const TeamStatus &status, const TeamMachine &machine);
QString teamCellLastSeen(const TeamStatus &status, const TeamMachine &machine);
QString teamCellSource(const TeamStatus &status, const TeamMachine &machine);
QString teamRowName(const TeamStatus &status, const TeamMachine &machine);

QString teamSummaryText(const TeamStatus &status, qint64 nowMs);
QString teamNoteText(const TeamStatus &status);
QString teamMachineDetailText(const TeamStatus &status,
                              const TeamMachine &machine);

// The three distinct causes.
QString teamLinkText(const TeamStatus &status, qint64 nowMs);
QString teamUnreachableText(const TeamStatus &status, qint64 nowMs);
QString teamNoMachinesText();
QString teamRefusalText(int httpStatus, const QString &detail);
// The body of a relay refusal, with the useful field picked out.
QString teamErrorDetail(const QByteArray &body);

Q_DECLARE_METATYPE(TeamStatus);
Q_DECLARE_METATYPE(TeamIncident);