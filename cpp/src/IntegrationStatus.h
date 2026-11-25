#pragma once

#include <QList>
#include <QString>

// The Integrations page answers the question a tile grid answers in a SaaS
// console: what is wired in, and what is not. Crowsnest compiles its
// integrations in, so the honest version is not a marketplace - it is a
// coverage board: every source this build can read, its state on THIS machine,
// one line of evidence, and the page that owns it.
//
// A state is one of:
//   LIVE    - data arrived from it (or it is this machine itself)
//   READY   - enabled and listening, but nothing has arrived yet
//   OFF     - a switch is off or nothing is configured
//   MISSING - the tool/engine/config it needs is not on this machine
//   GATED   - this OS does not expose the surface at all
struct IntegrationInputs {
    QString osName = QStringLiteral("linux");
    bool host = false;
    bool focus = false;
    QString focusProvider;
    bool journal = false;
    int journalLines = 0;
    bool services = false;
    int serviceUnits = 0;
    int serviceFailed = 0;
    bool sensors = false;
    int sensorChips = 0;
    bool gpu = false;
    int gpuDevices = 0;
    bool dockerAvailable = false;
    bool dockerEngine = false;
    int containersRunning = 0;
    bool kubeAvailable = false;
    int pods = 0;
    bool traces = false;
    int spans = 0;
    bool statsd = false;
    int series = 0;
    bool synthetic = false;
    int checks = 0;
    int checksDown = 0;
    bool tls = false;
    int tlsTargets = 0;
    bool dbm = false;
    QString dbName;
    bool cloud = false;
    QString cloudProvider;
    bool fleet = false;
    int peers = 0;
    bool relay = false;
    bool webhook = false;
    bool metrics = false;
    int labels = 0;
    bool history = false;
    int historySamples = 0;
};

struct IntegrationTile {
    QString group;
    QString name;
    QString state;
    QString evidence;
    // Rail page that owns the detail, empty when the source only lives in
    // Settings (there is no dedicated page for it).
    QString page;
};

QList<IntegrationTile> buildIntegrationTiles(const IntegrationInputs &inputs);
QString integrationSummaryText(const QList<IntegrationTile> &tiles);
