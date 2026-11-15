#pragma once

#include "LinuxProcess.h"

#include <QHash>
#include <QList>
#include <QString>

class SystemBackend;

struct ProcessNetRate {
    double rxPerSec = -1.0;
    double txPerSec = -1.0;
};

struct ProcessNetPrior {
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
    bool seen = false;
};

struct ProcessNetPidRate {
    double rxPerSec = -1.0;
    double txPerSec = -1.0;
    int sockets = 0;
};

// One row of "which process is talking to where": sockets are grouped by
// (process, protocol, remote endpoint, state) so a browser with forty sockets
// to one address is one line with a count instead of forty lines.
struct ProcessNetConnection {
    int pid = 0;
    QString name;
    QString proto;
    QString local;
    QString remote;
    QString state;
    int count = 1;
};

struct ProcessNetTalker {
    int pid = 0;
    int sockets = 0;
    double rxPerSec = 0.0;
    double txPerSec = 0.0;
    QString name;
    QList<quint16> listenPorts;
};

QHash<unsigned long long, ProcessNetRate> processNetSocketRates(
    const QList<linuxproc::TcpDiagInfo> &diag, qint64 intervalMs,
    QHash<unsigned long long, ProcessNetPrior> &prior);

QHash<int, ProcessNetPidRate> processNetPidRates(
    const QHash<unsigned long long, ProcessNetRate> &socketRates,
    const QHash<unsigned long long, int> &inodePid);

QHash<unsigned long long, int> processNetInodePid(SystemBackend &backend,
                                                 const QList<int> &pids);

QList<quint16> processNetListenPorts(const QList<linuxproc::SocketInfo> &sockets,
                                     const QList<unsigned long long> &inodes);

QList<ProcessNetConnection> processNetConnections(
    const QList<linuxproc::SocketInfo> &sockets,
    const QHash<unsigned long long, int> &inodePid,
    const QHash<int, QString> &commByPid, int limit);

QString formatEndpoint(const QString &addr, quint16 port);

QList<ProcessNetTalker> processNetTopTalkers(
    const QHash<int, ProcessNetPidRate> &pidRates, int limit);

QString formatNetRate(double bytesPerSec);
QString formatListenPorts(const QList<quint16> &ports);
QString processNetSummaryText(int attributed, int unattributed);
// The line under the connections table: what the table covers, and what it
// cannot see, named instead of silently missing.
QString processConnectionsNote(int rows, int unattributed);
