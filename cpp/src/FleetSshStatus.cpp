#include "FleetSshStatus.h"

#include "Config.h"

#include <QDateTime>
#include <QDebug>
#include <QProcess>
#include <QTimer>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kMaxConcurrentSsh = 4;
constexpr int kSshStartMs = 5000;
constexpr int kSshKillGraceMs = 500;

QString firstLine(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    return trimmed.section(QLatin1Char('\n'), 0, 0).trimmed();
}

QString sshHostFromSpec(const QString &spec) {
    QString text = spec.trimmed();
    const int equals = text.indexOf(QLatin1Char('='));
    if (equals > 0 && text.mid(equals + 1).startsWith(QLatin1String("ssh://"))) {
        text = text.mid(equals + 1).trimmed();
    }
    if (text.startsWith(QLatin1String("ssh://"))) {
        text = text.mid(6);
    }
    while (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !text.mid(colon + 1).contains(QLatin1Char('@'))) {
        bool ok = false;
        const int port = text.mid(colon + 1).toInt(&ok);
        if (ok && port > 0) {
            text = text.left(colon);
        }
    }
    return text;
}

QString sshNameFromSpec(const QString &spec) {
    const QString text = spec.trimmed();
    const int equals = text.indexOf(QLatin1Char('='));
    if (equals > 0 && text.mid(equals + 1).startsWith(QLatin1String("ssh://"))) {
        const QString name = text.left(equals).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    QString host = sshHostFromSpec(text);
    const int at = host.indexOf(QLatin1Char('@'));
    if (at >= 0) {
        host = host.mid(at + 1);
    }
    return host.isEmpty() ? text : host;
}

QString sshConnectionError(const QString &stderrText, int exitCode) {
    const QString text = stderrText.toLower();
    if (text.contains(QLatin1String("permission denied"))) {
        return QStringLiteral("permission denied - set up a key (ssh-copy-id)");
    }
    if (text.contains(QLatin1String("host key verification failed"))) {
        return QStringLiteral("host key changed - check the fingerprint, then "
                              "remove the old key from known_hosts");
    }
    if (text.contains(QLatin1String("connection refused"))) {
        return QStringLiteral("connection refused - is sshd listening?");
    }
    if (text.contains(QLatin1String("could not resolve hostname"))) {
        return QStringLiteral("could not resolve the hostname");
    }
    if (text.contains(QLatin1String("connection timed out")) ||
        text.contains(QLatin1String("timed out"))) {
        return QStringLiteral("timed out");
    }
    const QString first = firstLine(stderrText);
    if (!first.isEmpty()) {
        return first.left(160);
    }
    return QStringLiteral("ssh exited %1").arg(exitCode);
}

} // namespace

QString fleetSshCommand() {
    // One line, one shell. Markers keep the parse independent of proc's own
    // formatting; nothing is written on the remote host.
    return QStringLiteral(
        "awk '/^cpu /{print \"@@CPU\",$2,$3,$4,$5,$6,$7,$8,$9; exit}' "
        "/proc/stat; "
        "awk '/^MemTotal:/{print \"@@MEMT\",$2} "
        "/^MemAvailable:/{print \"@@MEMA\",$2}' /proc/meminfo; "
        "cut -d' ' -f1,2,3 /proc/loadavg | sed 's/^/@@LOAD /'; "
        "cut -d' ' -f1 /proc/uptime | sed 's/^/@@UP /'; "
        "printf '@@HOST '; hostname; "
        "printf '@@UNAME '; uname -sr");
}

FleetSshSample parseFleetSshSample(const QByteArray &body) {
    FleetSshSample sample;
    bool sawCpu = false;
    const QStringList lines =
        QString::fromUtf8(body).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("@@CPU"))) {
            const QStringList parts =
                line.mid(5).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() < 8) {
                continue;
            }
            quint64 total = 0;
            quint64 idle = 0;
            for (int i = 0; i < 8; ++i) {
                bool ok = false;
                const quint64 value = parts.at(i).toULongLong(&ok);
                if (!ok) {
                    total = 0;
                    break;
                }
                total += value;
                if (i == 3 || i == 4) {
                    idle += value;
                }
            }
            if (total > 0) {
                sample.cpuTotalJiffies = total;
                sample.cpuIdleJiffies = idle;
                sawCpu = true;
            }
        } else if (line.startsWith(QLatin1String("@@MEMT"))) {
            sample.memTotalKb = line.mid(7).trimmed().toULongLong();
        } else if (line.startsWith(QLatin1String("@@MEMA"))) {
            sample.memAvailableKb = line.mid(7).trimmed().toULongLong();
        } else if (line.startsWith(QLatin1String("@@LOAD"))) {
            const QStringList parts =
                line.mid(6).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (!parts.isEmpty()) {
                sample.load1 = parts.first().toDouble();
            }
        } else if (line.startsWith(QLatin1String("@@UP"))) {
            sample.uptimeSec =
                quint64(line.mid(4).trimmed().toDouble());
        } else if (line.startsWith(QLatin1String("@@HOST"))) {
            sample.hostname = line.mid(6).trimmed();
        } else if (line.startsWith(QLatin1String("@@UNAME"))) {
            sample.kernel = line.mid(7).trimmed();
        }
    }
    sample.ok = sawCpu && sample.memTotalKb > 0;
    if (!sample.ok && sample.error.isEmpty()) {
        sample.error = QStringLiteral(
            "the host did not answer with /proc/stat and /proc/meminfo - is it "
            "Linux?");
    }
    return sample;
}

double fleetSshCpuPercent(quint64 priorTotal, quint64 priorIdle,
                          quint64 total, quint64 idle) {
    if (priorTotal == 0 || total <= priorTotal || idle < priorIdle) {
        return -1.0; // first sample, a wrapped counter, or a reboot
    }
    const quint64 totalDelta = total - priorTotal;
    const quint64 idleDelta = idle - priorIdle;
    if (idleDelta > totalDelta) {
        return -1.0;
    }
    return 100.0 * double(totalDelta - idleDelta) / double(totalDelta);
}

QStringList fleetSshArguments(const QString &spec, int connectTimeoutSec) {
    QStringList args;
    args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
         << QStringLiteral("-o")
         << QStringLiteral("ConnectTimeout=%1")
                .arg(qBound(1, connectTimeoutSec, 60))
         << QStringLiteral("-o")
         << QStringLiteral("StrictHostKeyChecking=accept-new");
    QString text = spec.trimmed();
    const int equals = text.indexOf(QLatin1Char('='));
    if (equals > 0 && text.mid(equals + 1).startsWith(QLatin1String("ssh://"))) {
        text = text.mid(equals + 1).trimmed();
    }
    if (text.startsWith(QLatin1String("ssh://"))) {
        text = text.mid(6);
    }
    while (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    int port = 0;
    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !text.mid(colon + 1).contains(QLatin1Char('@'))) {
        bool ok = false;
        const int parsed = text.mid(colon + 1).toInt(&ok);
        if (ok && parsed > 0) {
            port = parsed;
            text = text.left(colon);
        }
    }
    if (port > 0) {
        args << QStringLiteral("-p") << QString::number(port);
    }
    args << text << fleetSshCommand();
    return args;
}

FleetPeer fleetSshPeer(const QString &spec, QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    FleetPeer peer;
    peer.address = spec;
    peer.agentless = true;
    peer.name = sshNameFromSpec(spec);
    if (sshHostFromSpec(spec).isEmpty()) {
        peer.configured = false;
        peer.configNote = QStringLiteral("expected ssh://user@host");
        if (error != nullptr) {
            *error = peer.configNote;
        }
    }
    return peer;
}

QList<FleetPeer> probeFleetSsh(const FleetSshOptions &options, qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    QList<FleetPeer> peers;
    if (!options.enabled) {
        return peers;
    }
    for (const QString &spec : options.peers) {
        FleetPeer peer = fleetSshPeer(spec);
        if (!peer.configured) {
            peers.append(peer);
            continue;
        }
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setProgram(QStringLiteral("ssh"));
        process.setArguments(
            fleetSshArguments(spec, qMax(1, options.timeoutMs / 1000)));
        QElapsedTimer clock;
        clock.start();
        process.start();
        if (!process.waitForStarted(kSshStartMs)) {
            peer.online = false;
            peer.error = QStringLiteral("could not start ssh (%1)")
                             .arg(process.errorString());
            peers.append(peer);
            continue;
        }
        if (!process.waitForFinished(qBound(1000, options.timeoutMs, 60000))) {
            process.kill();
            process.waitForFinished(kSshKillGraceMs);
            peer.online = false;
            peer.error = QStringLiteral("timed out");
            peers.append(peer);
            continue;
        }
        peer.latencyMs = int(clock.elapsed());
        peer.lastAttemptMs = nowMs;
        const FleetSshSample sample = parseFleetSshSample(
            process.readAllStandardOutput());
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0 || !sample.ok) {
            peer.online = false;
            // The exit code is the first fact: ssh said why before the body
            // was even a question, and a refused key must not read as "not
            // Linux".
            peer.error = (process.exitStatus() != QProcess::NormalExit ||
                          process.exitCode() != 0)
                             ? sshConnectionError(
                                   QString::fromLocal8Bit(
                                       process.readAllStandardError()),
                                   process.exitCode())
                             : sample.error;
            peers.append(peer);
            continue;
        }
        peer.online = true;
        peer.everSeen = true;
        peer.lastSeenMs = nowMs;
        peer.hostname = sample.hostname;
        peer.platform = QStringLiteral("linux");
        peer.version = sample.kernel.isEmpty()
                           ? QStringLiteral("agentless (ssh)")
                           : QStringLiteral("agentless (ssh, %1)")
                                 .arg(sample.kernel);
        peer.uptimeSec = qint64(sample.uptimeSec);
        peer.memTotalKb = sample.memTotalKb;
        peer.memAvailableKb = sample.memAvailableKb;
        peer.load1 = sample.load1;
        peers.append(peer);
    }
    return peers;
}

FleetSshRunner::FleetSshRunner(QObject *parent)
    : QObject(parent) {
}

FleetSshRunner::~FleetSshRunner() {
    stop();
}

void FleetSshRunner::setOptions(const FleetSshOptions &options) {
    m_options = options;
}

void FleetSshRunner::start() {
    if (m_timer == nullptr) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::CoarseTimer);
        connect(m_timer, &QTimer::timeout, this,
                [this]() { update(QDateTime::currentMSecsSinceEpoch()); });
    }
    // One ssh per peer per poll; the poll interval is the fleet cadence but a
    // ten second floor keeps a fast cadence from spawning back to back logins.
    // The timer wakes twice per period: a coarse timer can fire a few percent
    // early, and a due check that misses by 50 ms would wait a whole period.
    m_timer->setInterval(qMax(5000, m_options.pollMs / 2));
    m_timer->start();
    update(QDateTime::currentMSecsSinceEpoch());
}

void FleetSshRunner::pollNow() {
    m_lastPollMs = 0;
    update(QDateTime::currentMSecsSinceEpoch());
}

void FleetSshRunner::cleanup() {
    for (Job &job : m_jobs) {
        if (job.process == nullptr) {
            continue;
        }
        if (job.process->state() != QProcess::NotRunning) {
            job.process->terminate();
            if (!job.process->waitForFinished(kSshKillGraceMs)) {
                job.process->kill();
                job.process->waitForFinished(kSshKillGraceMs);
            }
        }
        delete job.process;
        job.process = nullptr;
    }
    m_jobs.clear();
}

void FleetSshRunner::stop() {
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    cleanup();
}

void FleetSshRunner::update(qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (!m_options.enabled || m_options.peers.isEmpty()) {
        if (!m_peers.isEmpty() || !m_jobs.isEmpty()) {
            cleanup();
            m_peers.clear();
            m_lastPollMs = 0;
            emit peersReady(m_peers);
        }
        return;
    }
    poll(nowMs);
    if (m_jobs.isEmpty() && (m_lastPollMs == 0 ||
                             nowMs - m_lastPollMs >= m_options.pollMs)) {
        begin(nowMs);
    }
}

void FleetSshRunner::begin(qint64 nowMs) {
    m_lastPollMs = nowMs;
    for (const QString &spec : m_options.peers) {
        if (m_jobs.size() >= kMaxConcurrentSsh) {
            break;
        }
        FleetPeer seed = fleetSshPeer(spec);
        if (!seed.configured) {
            // Keep the refusal on screen; it is a configuration error the user
            // has to see.
            bool replaced = false;
            for (FleetPeer &peer : m_peers) {
                if (peer.address == spec) {
                    peer = seed;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_peers.append(seed);
            }
            continue;
        }
        // Seed the row immediately: a configured host that never answers is
        // visible as "connecting" for the length of its budget instead of
        // being missing from the table. A host that has answered before keeps
        // its last readings while the next poll is in flight - the AGE column
        // dates them, and only a failed poll dashes them.
        seed.connecting = true;
        bool seeded = false;
        for (FleetPeer &peer : m_peers) {
            if (peer.address == spec) {
                if (!peer.everSeen) {
                    peer.connecting = true;
                }
                seeded = true;
                break;
            }
        }
        if (!seeded) {
            m_peers.append(seed);
        }
        auto *process = new QProcess();
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setProgram(QStringLiteral("ssh"));
        process->setArguments(fleetSshArguments(
            spec, qMax(1, m_options.timeoutMs / 1000)));
        process->start();
        if (!process->waitForStarted(kSshStartMs)) {
            delete process;
            FleetPeer failed = seed;
            failed.online = false;
            failed.lastAttemptMs = nowMs;
            failed.error = QStringLiteral("could not start ssh (%1)")
                               .arg(QStringLiteral("not on PATH"));
            bool replaced = false;
            for (FleetPeer &peer : m_peers) {
                if (peer.address == spec) {
                    peer = failed;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_peers.append(failed);
            }
            continue;
        }
        // An ssh call finishes in tens of milliseconds but the poll timer is
        // ten seconds away: handle the answer when it lands, or the row reads
        // "connecting" for the whole interval. Posted through the event loop
        // because handle() deletes the process whose signal we are in.
        connect(process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this]() {
                    QTimer::singleShot(0, this, [this]() {
                        poll(QDateTime::currentMSecsSinceEpoch());
                    });
                });
        Job job;
        job.process = process;
        job.spec = spec;
        job.name = seed.name;
        job.startedMs = nowMs;
        m_jobs.append(job);
    }
    emit peersReady(m_peers);
}

void FleetSshRunner::poll(qint64 nowMs) {
    bool changed = false;
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        Job &job = m_jobs[i];
        if (job.process->state() != QProcess::NotRunning) {
            if (job.startedMs > 0 &&
                nowMs - job.startedMs > qint64(m_options.timeoutMs)) {
                job.process->terminate();
                if (!job.process->waitForFinished(kSshKillGraceMs)) {
                    job.process->kill();
                    job.process->waitForFinished(kSshKillGraceMs);
                }
            } else {
                continue;
            }
        }
        handle(job, nowMs);
        delete job.process;
        job.process = nullptr;
        m_jobs.removeAt(i);
        changed = true;
    }
    if (changed) {
        emit peersReady(m_peers);
    }
}

void FleetSshRunner::handle(const Job &job, qint64 nowMs) {
    const QByteArray out = job.process->readAllStandardOutput();
    const QByteArray err = job.process->readAllStandardError();
    const int exitCode = job.process->exitCode();
    const bool normalExit =
        job.process->exitStatus() == QProcess::NormalExit;
    const FleetSshSample sample = parseFleetSshSample(out);

    FleetPeer peer = fleetSshPeer(job.spec);
    peer.lastAttemptMs = nowMs;
    peer.connecting = false;
    if (!normalExit || exitCode != 0 || !sample.ok) {
        peer.online = false;
        peer.error = (!normalExit || exitCode != 0)
                         ? sshConnectionError(QString::fromLocal8Bit(err),
                                              exitCode)
                         : sample.error;
        // Keep the last known values: the page dates them and dashes the
        // readings of a peer that is down.
        for (const FleetPeer &old : m_peers) {
            if (old.address == job.spec) {
                peer.everSeen = old.everSeen;
                peer.lastSeenMs = old.lastSeenMs;
                break;
            }
        }
    } else {
        peer.online = true;
        peer.everSeen = true;
        peer.lastSeenMs = nowMs;
        peer.hostname = sample.hostname;
        peer.platform = QStringLiteral("linux");
        peer.version = sample.kernel.isEmpty()
                           ? QStringLiteral("agentless (ssh)")
                           : QStringLiteral("agentless (ssh, %1)")
                                 .arg(sample.kernel);
        peer.uptimeSec = qint64(sample.uptimeSec);
        peer.memTotalKb = sample.memTotalKb;
        peer.memAvailableKb = sample.memAvailableKb;
        peer.load1 = sample.load1;
        peer.latencyMs = int(qMax<qint64>(0, nowMs - job.startedMs));
        const auto prior = m_prior.constFind(job.spec);
        if (prior != m_prior.constEnd()) {
            peer.cpuPercent = fleetSshCpuPercent(prior.value().first,
                                                 prior.value().second,
                                                 sample.cpuTotalJiffies,
                                                 sample.cpuIdleJiffies);
        }
        m_prior.insert(job.spec,
                       {sample.cpuTotalJiffies, sample.cpuIdleJiffies});
    }

    bool replaced = false;
    for (FleetPeer &existing : m_peers) {
        if (existing.address == job.spec) {
            existing = peer;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_peers.append(peer);
    }
}
