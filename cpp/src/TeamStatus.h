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
    // M161: the machine's own labels, as the relay stored them (M160). Empty
    // when the machine has none, or when the relay predates the column - the
    // page says "no labels" rather than showing a blank cell either way.
    QList<QPair<QString, QString>> labels;
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
struct TeamStatus;

// M177: the relay's own checks (HTTP/TCP/TLS/push), read-only on this page.
// The relay measures; the desktop only renders the answer, so a check cannot
// disagree with itself between the browser and the HUD.
// ---------------------------------------------------------------------------

struct TeamCheck {
    QString id;
    QString name;
    QString kind;   // http | tcp | tls | push
    QString target;
    QString state;  // up | down | unknown
    bool enabled = false;
    bool heartbeatSet = false;
    int failing = 0;
    int lastStatus = 0;
    int lastLatencyMs = 0;
    int intervalS = 0;
    int graceS = 0;
    qint64 lastCheckedAtMs = -1;
    qint64 lastOkAtMs = -1;
    qint64 lastPushedAtMs = -1;
    qint64 certExpiresAtMs = -1;
    QString lastError;
    QList<QPair<QString, QString>> labels;

    bool push() const { return kind == QLatin1String("push"); }
};

// The shape `GET /api/v1/checks` answers. Same schema integer as the fleet
// read; a newer answer is refused with the fix named.
bool teamParseChecksJson(const QByteArray &body, TeamStatus *status,
                         QString *error);

QString teamCheckSummaryText(const TeamStatus &status);
QString teamCheckStateText(const TeamCheck &check);
QString teamCheckTargetText(const TeamCheck &check);
QString teamCheckLastText(const TeamStatus &status, const TeamCheck &check);
QString teamCheckLatencyText(const TeamCheck &check);
QString teamCheckCertText(const TeamStatus &status, const TeamCheck &check);
QString teamCheckDetailText(const TeamStatus &status, const TeamCheck &check);
int teamCheckCertDays(const TeamStatus &status, const TeamCheck &check);

// ---------------------------------------------------------------------------
// M152: the relay's incident work queue and the three verbs the HUD may send.
// PagerDuty's model is the one the relay already speaks: triggered/acknowledged/
// resolved, an acknowledgement list with who and when, and `assignments` empty
// once resolved. The HUD never invents an owner: the row prints whoever the
// relay says holds it.
// ---------------------------------------------------------------------------

enum class TeamAction { Ack, Close, Silence, SilenceLabel };

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

// M169: a silence keyed by a machine label (`scope = label` on the relay).
// `matcher` is one `key=value` pair, because the relay validates it with the
// same rules it applies to ingest labels; a malformed pair is refused here so
// the button cannot post a silence that mutes nothing (or everything).
QByteArray teamLabelSilenceBody(const QString &matcher, const QString &actor,
                                int minutes, qint64 nowMs);

// A workspace status page, read through the relay's public JSON route. The
// desktop previews what customers would see: components, their 30-day
// coverage, the incidents that are open and the ones that resolved. The
// token in [remote] status_token is the whole credential.
struct TeamStatusComponent {
    QString name;
    QString unit;
    QString state;
    int up = 0;
    int total = 0;
    double coverage = -1.0;
    QString coverageText;
};

struct TeamStatusIncident {
    QString title;
    QString severity;
    QString component;
    QString opened;
    QString duration;
    QString state;
};

struct TeamStatusPage {
    bool configured = false;
    bool answered = false;
    QString error;
    QString name;
    QString state;
    qint64 asOfMs = -1;
    bool includeChecks = false;
    QList<TeamStatusComponent> components;
    QList<TeamStatusIncident> active;
    QList<TeamStatusIncident> resolved;
};

bool teamParseStatusPageJson(const QByteArray &body, TeamStatusPage *page,
                             QString *error);
QString teamStatusPageSummaryText(const TeamStatusPage &page);
QString teamStatusPageComponentText(const TeamStatusComponent &component);

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

    // The relay's own probes (M177). `checksError` is a separate sentence so a
    // fleet answer is still worth showing when checks are refused or an older
    // relay does not have them.
    bool checksAnswered = false;
    QString checksError;
    qint64 checksAsOfMs = -1;
    QList<TeamCheck> checks;
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
// M161: "role=web env=prod" in key order, or "no labels".
QString teamLabelsText(const TeamMachine &machine);
// Every distinct key=value across the relay's fleet, sorted, for the page's
// filter - the same shape the Fleet page uses.
QStringList teamLabelFacets(const QList<TeamMachine> &machines);

// M169: grouping the relay's fleet by one authored label. `value` is
// "(no <key>)" for a machine that does not carry it, so the row counts add up
// to the filter's own total; `members` are indexes into the list that was
// grouped, which is what the table renders.
struct TeamGroup {
    QString key;
    QString value;
    QList<int> members;
    int online = 0;
    int silent = 0;
};
QStringList teamLabelKeys(const QList<TeamMachine> &machines);
QList<TeamGroup> teamGroups(const QList<TeamMachine> &machines,
                            const QString &key);
QString teamGroupRowText(const TeamGroup &group);
// "3 of 4 machine(s) labelled - 2 team(s) - 2 env(s)" for the summary line.
QString teamLabelsSummaryText(const QList<TeamMachine> &machines);
QString teamLabelValue(const TeamMachine &machine, const QString &key);
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