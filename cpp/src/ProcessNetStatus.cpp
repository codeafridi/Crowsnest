#include "ProcessNetStatus.h"

#include "platform/SystemBackend.h"

#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kPriorCap = 4096;

struct Accumulator {
    double rx = 0.0;
    double tx = 0.0;
    int sockets = 0;
    bool known = false;
};

} // namespace

QHash<unsigned long long, ProcessNetRate> processNetSocketRates(
    const QList<linuxproc::TcpDiagInfo> &diag, qint64 intervalMs,
    QHash<unsigned long long, ProcessNetPrior> &prior) {

    QHash<unsigned long long, ProcessNetRate> rates;
    if (prior.size() > kPriorCap) {
        prior.clear();
    }
    const bool haveWindow = intervalMs > 0;
    const double seconds =
        haveWindow ? static_cast<double>(intervalMs) / 1000.0 : 0.0;

    for (const linuxproc::TcpDiagInfo &info : diag) {
        if (!info.hasInfo || info.inode == 0) {
            continue;
        }
        const quint64 rx = info.bytesReceived;
        const quint64 tx = info.bytesSent;
        ProcessNetRate rate;
        ProcessNetPrior &slot = prior[info.inode];
        if (!slot.seen) {
            // No baseline yet: this tick cannot carry a rate.
            slot.seen = true;
        } else if (haveWindow && rx >= slot.rxBytes && tx >= slot.txBytes) {
            rate.rxPerSec = static_cast<double>(rx - slot.rxBytes) / seconds;
            rate.txPerSec = static_cast<double>(tx - slot.txBytes) / seconds;
        } else {
            // Counters moved backwards: the inode was recycled by a new
            // socket. Re-base and report an honest zero for this tick.
            rate.rxPerSec = 0.0;
            rate.txPerSec = 0.0;
        }
        slot.rxBytes = rx;
        slot.txBytes = tx;
        rates.insert(info.inode, rate);
    }
    return rates;
}

QHash<int, ProcessNetPidRate> processNetPidRates(
    const QHash<unsigned long long, ProcessNetRate> &socketRates,
    const QHash<unsigned long long, int> &inodePid) {

    QHash<int, Accumulator> acc;
    for (auto it = socketRates.constBegin(); it != socketRates.constEnd();
         ++it) {
        const auto pidIt = inodePid.constFind(it.key());
        if (pidIt == inodePid.constEnd()) {
            continue;
        }
        Accumulator &slot = acc[pidIt.value()];
        slot.sockets += 1;
        if (it->rxPerSec >= 0.0 && it->txPerSec >= 0.0) {
            slot.rx += it->rxPerSec;
            slot.tx += it->txPerSec;
            slot.known = true;
        }
    }

    QHash<int, ProcessNetPidRate> out;
    for (auto it = acc.constBegin(); it != acc.constEnd(); ++it) {
        ProcessNetPidRate row;
        row.sockets = it->sockets;
        if (it->known) {
            row.rxPerSec = it->rx;
            row.txPerSec = it->tx;
        }
        out.insert(it.key(), row);
    }
    return out;
}

QHash<unsigned long long, int> processNetInodePid(SystemBackend &backend,
                                                 const QList<int> &pids) {
    QHash<unsigned long long, int> map;
    for (int pid : pids) {
        const QList<unsigned long long> inodes = backend.socketInodes(pid);
        for (unsigned long long inode : inodes) {
            if (inode != 0) {
                map.insert(inode, pid);
            }
        }
    }
    return map;
}

QString formatEndpoint(const QString &addr, quint16 port) {
    QString host = addr.trimmed();
    if (host.isEmpty() || host == QLatin1String("0.0.0.0") ||
        host == QLatin1String("::") || host == QLatin1String("*")) {
        host = QStringLiteral("*");
    }
    if (port == 0) {
        return host;
    }
    if (host.contains(QLatin1Char(':'))) {
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    }
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

QList<ProcessNetConnection> processNetConnections(
    const QList<linuxproc::SocketInfo> &sockets,
    const QHash<unsigned long long, int> &inodePid,
    const QHash<int, QString> &commByPid, int limit) {
    QList<ProcessNetConnection> rows;
    if (limit <= 0) {
        return rows;
    }
    // Some kernels no longer export a UDP remote endpoint; a listening socket
    // (or a wildcard peer) has nowhere to go, so it reads as "*".
    QHash<QString, int> index;
    for (const linuxproc::SocketInfo &socket : sockets) {
        if (socket.inode == 0) {
            continue; // kernel sockets have no owner
        }
        const auto owner = inodePid.constFind(socket.inode);
        if (owner == inodePid.constEnd()) {
            continue; // another user's socket is not readable
        }
        ProcessNetConnection row;
        row.pid = owner.value();
        row.name = commByPid.value(row.pid);
        if (row.name.isEmpty()) {
            row.name = QStringLiteral("pid %1").arg(row.pid);
        }
        row.proto = socket.proto;
        row.local = formatEndpoint(socket.localAddr, socket.localPort);
        row.remote = formatEndpoint(socket.remoteAddr, socket.remotePort);
        row.state = socket.state;
        // Grouped by process and remote endpoint. The local ephemeral port is
        // deliberately not part of the key (forty sockets to one host are one
        // row); a listener is, because ports 80 and 443 are different things.
        QString key = QStringLiteral("%1|%2|%3|%4")
                          .arg(row.pid)
                          .arg(row.proto, row.remote, row.state);
        if (row.state == QLatin1String("LISTEN")) {
            key += QLatin1Char('|') + row.local;
        }
        const auto seen = index.constFind(key);
        if (seen != index.constEnd()) {
            ++rows[seen.value()].count;
            continue;
        }
        if (rows.size() >= limit) {
            continue;
        }
        index.insert(key, int(rows.size()));
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const ProcessNetConnection &left,
                 const ProcessNetConnection &right) {
                  const int byName =
                      left.name.compare(right.name, Qt::CaseInsensitive);
                  if (byName != 0) {
                      return byName < 0;
                  }
                  if (left.pid != right.pid) {
                      return left.pid < right.pid;
                  }
                  const int byRemote =
                      left.remote.compare(right.remote, Qt::CaseInsensitive);
                  if (byRemote != 0) {
                      return byRemote < 0;
                  }
                  return left.local.compare(right.local, Qt::CaseInsensitive) <
                         0;
              });
    return rows;
}

QList<quint16> processNetListenPorts(const QList<linuxproc::SocketInfo> &sockets,
                                     const QList<unsigned long long> &inodes) {
    QList<quint16> ports;
    if (inodes.isEmpty()) {
        return ports;
    }
    QSet<unsigned long long> wanted;
    for (unsigned long long inode : inodes) {
        wanted.insert(inode);
    }
    for (const linuxproc::SocketInfo &socket : sockets) {
        if (socket.state != QStringLiteral("LISTEN") || socket.inode == 0) {
            continue;
        }
        if (socket.localPort == 0 || !wanted.contains(socket.inode)) {
            continue;
        }
        if (!ports.contains(socket.localPort)) {
            ports.append(socket.localPort);
        }
    }
    std::sort(ports.begin(), ports.end());
    return ports;
}

QList<ProcessNetTalker> processNetTopTalkers(
    const QHash<int, ProcessNetPidRate> &pidRates, int limit) {

    QList<ProcessNetTalker> rows;
    for (auto it = pidRates.constBegin(); it != pidRates.constEnd(); ++it) {
        const double rx = qMax(0.0, it->rxPerSec);
        const double tx = qMax(0.0, it->txPerSec);
        if (it->rxPerSec < 0.0 && it->txPerSec < 0.0) {
            continue;
        }
        if (rx + tx <= 0.0) {
            continue;
        }
        ProcessNetTalker talker;
        talker.pid = it.key();
        talker.sockets = it->sockets;
        talker.rxPerSec = rx;
        talker.txPerSec = tx;
        rows.append(talker);
    }

    std::sort(rows.begin(), rows.end(),
              [](const ProcessNetTalker &lhs, const ProcessNetTalker &rhs) {
                  const double left = lhs.rxPerSec + lhs.txPerSec;
                  const double right = rhs.rxPerSec + rhs.txPerSec;
                  if (left != right) {
                      return left > right;
                  }
                  return lhs.pid < rhs.pid;
              });

    if (limit > 0 && rows.size() > limit) {
        rows.resize(limit);
    }
    return rows;
}

QString formatNetRate(double bytesPerSec) {
    if (!(bytesPerSec >= 0.0)) {
        return QStringLiteral("--");
    }
    if (bytesPerSec < 1024.0) {
        return QStringLiteral("%1 B/s").arg(qRound(bytesPerSec));
    }
    const double kb = bytesPerSec / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB/s").arg(qRound(kb));
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB/s").arg(mb, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB/s").arg(mb / 1024.0, 0, 'f', 2);
}

QString formatListenPorts(const QList<quint16> &ports) {
    if (ports.isEmpty()) {
        return QStringLiteral("--");
    }
    const int maxShown = 3;
    QStringList shown;
    for (int i = 0; i < ports.size() && i < maxShown; ++i) {
        shown.append(QString::number(ports.at(i)));
    }
    QString text = shown.join(QStringLiteral(", "));
    if (ports.size() > maxShown) {
        text += QStringLiteral(" +%1").arg(ports.size() - maxShown);
    }
    return text;
}

QString processConnectionsNote(int rows, int unattributed) {
    QString text = QStringLiteral(
        "%1 grouped endpoint(s), tcp only; a wildcard peer is a listener")
                       .arg(rows);
    if (unattributed > 0) {
        text += QStringLiteral(
                    " - %1 socket(s) belong to other users and are not "
                    "readable unprivileged")
                    .arg(unattributed);
    }
    return text;
}

QString processNetSummaryText(int attributed, int unattributed) {
    if (attributed <= 0 && unattributed <= 0) {
        return QStringLiteral("no tcp sockets");
    }
    return QStringLiteral("tcp sockets: %1 tracked, %2 other users")
        .arg(attributed)
        .arg(unattributed);
}
