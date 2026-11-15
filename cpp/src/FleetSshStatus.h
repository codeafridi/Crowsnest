#pragma once

#include "FleetStatus.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

// Agentless fleet peers: a Linux box you can ssh into, with nothing installed
// on it.
//
// The fleet page already polls peers that run Crowsnest. A server you cannot
// install on - a friend's VPS, a production box you only have an account on, a
// machine whose distro you do not want to touch - could not be watched at all.
// One non-interactive ssh call that cats four /proc files answers the same
// questions for the host: CPU, memory, load. It cannot see processes, disks or
// containers, and the row says so.
//
// Grounding: the four proc files are the same ones the Linux backend already
// parses (proc/stat, meminfo, loadavg, uptime), and `sshd` refuses a
// non-interactive login without a key, which is exactly the authentication
// model this uses (BatchMode). Nothing is installed, written or left behind on
// the remote host; the command is one `sh` line that reads files.
struct FleetSshSample {
    bool ok = false;
    QString error;
    // /proc/stat "cpu" line, in jiffies: total = user+nice+system+idle+
    // iowait+irq+softirq+steal, idle = idle+iowait.
    quint64 cpuTotalJiffies = 0;
    quint64 cpuIdleJiffies = 0;
    quint64 memTotalKb = 0;
    quint64 memAvailableKb = 0;
    double load1 = -1.0;
    quint64 uptimeSec = 0;
    QString hostname;
    QString kernel;
};

FleetSshSample parseFleetSshSample(const QByteArray &body);

// CPU percent between two /proc/stat readings. Returns -1 for the first
// sample, for a counter reset, and for a delta that cannot be a percentage -
// never a guessed number.
double fleetSshCpuPercent(quint64 priorTotal, quint64 priorIdle,
                          quint64 total, quint64 idle);

// The remote line. One `sh` command, no quoting traps on our side: ssh passes
// the single argument to the remote shell unchanged.
QString fleetSshCommand();

// `ssh://[user@]host[:port]` (optionally behind name=) -> the arguments to
// hand to ssh, and the peer row the page shows before the first answer.
QStringList fleetSshArguments(const QString &spec, int connectTimeoutSec);
FleetPeer fleetSshPeer(const QString &spec, QString *error = nullptr);

struct FleetSshOptions {
    bool enabled = false;
    // Only the ssh:// entries of [fleet] peers=.
    QStringList peers;
    int timeoutMs = 5000;
    int pollMs = kFleetDefaultPollMs;
};

// Synchronous one-shot, used by --dump. Blocks for up to timeoutMs per peer.
QList<FleetPeer> probeFleetSsh(const FleetSshOptions &options,
                               qint64 nowMs = 0);

class FleetSshRunner : public QObject {
    Q_OBJECT

public:
    explicit FleetSshRunner(QObject *parent = nullptr);
    ~FleetSshRunner() override;

    void setOptions(const FleetSshOptions &options);

public slots:
    // Called from the runner's own thread (started signal): one timer, one
    // poll at a time, so a slow ssh never stacks up.
    void start();
    void pollNow();
    void stop();

public:
    void update(qint64 nowMs);

    const QList<FleetPeer> &peers() const { return m_peers; }

signals:
    void peersReady(const QList<FleetPeer> &peers);

private:
    struct Job {
        QProcess *process = nullptr;
        QString spec;
        QString name;
        qint64 startedMs = 0;
    };

    void begin(qint64 nowMs);
    void poll(qint64 nowMs);
    void handle(const Job &job, qint64 nowMs);
    void cleanup();

    FleetSshOptions m_options;
    QList<FleetPeer> m_peers;
    QTimer *m_timer = nullptr;
    QList<Job> m_jobs;
    // Previous CPU jiffies per peer, for the percentage between two polls.
    QHash<QString, QPair<quint64, quint64>> m_prior;
    qint64 m_lastPollMs = 0;
};
