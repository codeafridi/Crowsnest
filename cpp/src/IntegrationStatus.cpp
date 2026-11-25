#include "IntegrationStatus.h"

#include <QMap>

namespace {

IntegrationTile tile(const QString &group, const QString &name,
                     const QString &state, const QString &evidence,
                     const QString &page = QString()) {
    IntegrationTile out;
    out.group = group;
    out.name = name;
    out.state = state;
    out.evidence = evidence;
    out.page = page;
    return out;
}

QString gatedText(const QString &os, const QString &surface) {
    return QStringLiteral("%1 does not expose %2").arg(os, surface);
}

} // namespace

QList<IntegrationTile> buildIntegrationTiles(const IntegrationInputs &in) {
    QList<IntegrationTile> tiles;

    // --- Agent and host -------------------------------------------------
    tiles << tile(QStringLiteral("Agent & host"), QStringLiteral("Host agent"),
                  in.host ? QStringLiteral("LIVE") : QStringLiteral("READY"),
                  in.host ? QStringLiteral("collecting on %1").arg(in.osName)
                          : QStringLiteral("waiting for the first sample"),
                  QStringLiteral("Host"));
    tiles << tile(QStringLiteral("Agent & host"),
                  QStringLiteral("Focus provider"),
                  in.focus ? QStringLiteral("LIVE") : QStringLiteral("GATED"),
                  in.focus && !in.focusProvider.isEmpty()
                      ? QStringLiteral("via %1").arg(in.focusProvider)
                      : gatedText(in.osName, QStringLiteral("a focused window")),
                  QStringLiteral("Processes"));
    tiles << tile(QStringLiteral("Agent & host"), QStringLiteral("Journald"),
                  in.journal ? QStringLiteral("LIVE") : QStringLiteral("GATED"),
                  in.journal ? QStringLiteral("%1 line(s) in the window")
                                   .arg(in.journalLines)
                             : gatedText(in.osName, QStringLiteral("journald")),
                  QStringLiteral("Logs"));
    tiles << tile(QStringLiteral("Agent & host"), QStringLiteral("Systemd"),
                  in.services ? QStringLiteral("LIVE")
                              : QStringLiteral("GATED"),
                  in.services ? QStringLiteral("%1 unit(s), %2 failed")
                                    .arg(in.serviceUnits)
                                    .arg(in.serviceFailed)
                              : gatedText(in.osName,
                                          QStringLiteral("systemd services")),
                  QStringLiteral("Services"));
    tiles << tile(QStringLiteral("Agent & host"), QStringLiteral("Sensors"),
                  in.sensors ? QStringLiteral("LIVE") : QStringLiteral("GATED"),
                  in.sensors ? QStringLiteral("%1 chip(s)").arg(in.sensorChips)
                             : gatedText(in.osName, QStringLiteral("hwmon")),
                  QStringLiteral("Sensors"));
    tiles << tile(QStringLiteral("Agent & host"), QStringLiteral("GPU"),
                  in.gpu ? QStringLiteral("LIVE") : QStringLiteral("GATED"),
                  in.gpu ? QStringLiteral("%1 device(s)").arg(in.gpuDevices)
                         : gatedText(in.osName,
                                     QStringLiteral("a GPU counter")),
                  QStringLiteral("GPU"));

    // --- Workloads ------------------------------------------------------
    QString dockerState = QStringLiteral("MISSING");
    QString dockerNote = QStringLiteral("no Docker engine or CLI found");
    if (in.dockerEngine && in.containersRunning > 0) {
        dockerState = QStringLiteral("LIVE");
        dockerNote = QStringLiteral("%1 container(s) running")
                         .arg(in.containersRunning);
    } else if (in.dockerEngine) {
        dockerState = QStringLiteral("READY");
        dockerNote = QStringLiteral("engine answers, nothing running");
    } else if (in.dockerAvailable) {
        dockerState = QStringLiteral("READY");
        dockerNote = QStringLiteral("docker CLI found, engine unreachable");
    }
    tiles << tile(QStringLiteral("Workloads"), QStringLiteral("Docker"),
                  dockerState, dockerNote, QStringLiteral("Containers"));

    tiles << tile(QStringLiteral("Workloads"), QStringLiteral("Kubernetes"),
                  in.kubeAvailable ? QStringLiteral("LIVE")
                                   : QStringLiteral("MISSING"),
                  in.kubeAvailable
                      ? QStringLiteral("%1 pod(s) in the current context")
                            .arg(in.pods)
                      : QStringLiteral("no kubeconfig or cluster reachable"),
                  QStringLiteral("Kubernetes"));

    tiles << tile(QStringLiteral("Workloads"), QStringLiteral("PostgreSQL"),
                  in.dbm ? QStringLiteral("LIVE") : QStringLiteral("OFF"),
                  in.dbm ? (in.dbName.isEmpty()
                                ? QStringLiteral("pg_stat views answering")
                                : QStringLiteral("database %1").arg(in.dbName))
                         : QStringLiteral("no [database] dsn configured"),
                  QStringLiteral("Databases"));

    // --- Telemetry in ---------------------------------------------------
    // "available" only means the receiver is up. LIVE has to mean data
    // arrived, or a fresh idle box reads as if spans were flowing.
    const bool tracesLive = in.traces && in.spans > 0;
    tiles << tile(QStringLiteral("Telemetry in"), QStringLiteral("OTLP traces"),
                  tracesLive ? QStringLiteral("LIVE")
                             : QStringLiteral("READY"),
                  tracesLive
                      ? QStringLiteral("%1 span(s) received").arg(in.spans)
                      : QStringLiteral("listening on 127.0.0.1:4318, "
                                       "nothing received yet"),
                  QStringLiteral("Traces"));
    const bool statsdLive = in.statsd && in.series > 0;
    tiles << tile(QStringLiteral("Telemetry in"),
                  QStringLiteral("DogStatsD"),
                  statsdLive ? QStringLiteral("LIVE")
                             : QStringLiteral("READY"),
                  statsdLive
                      ? QStringLiteral("%1 series").arg(in.series)
                      : QStringLiteral("listening on 127.0.0.1:8125, "
                                       "nothing received yet"),
                  QStringLiteral("Metrics"));
    tiles << tile(QStringLiteral("Telemetry in"),
                  QStringLiteral("Synthetic checks"),
                  in.checks > 0 ? QStringLiteral("LIVE")
                                : QStringLiteral("READY"),
                  in.checks > 0
                      ? QStringLiteral("%1 target(s), %2 down")
                            .arg(in.checks)
                            .arg(in.checksDown)
                      : QStringLiteral("no [synthetic] targets"),
                  QStringLiteral("Synthetic"));
    tiles << tile(QStringLiteral("Telemetry in"),
                  QStringLiteral("TLS certificates"),
                  in.tlsTargets > 0 ? QStringLiteral("LIVE")
                                    : QStringLiteral("READY"),
                  in.tlsTargets > 0
                      ? QStringLiteral("%1 target(s)").arg(in.tlsTargets)
                      : QStringLiteral("no [tls] targets"),
                  QStringLiteral("Network"));
    tiles << tile(QStringLiteral("Telemetry in"), QStringLiteral("Webhook"),
                  in.webhook ? QStringLiteral("LIVE") : QStringLiteral("OFF"),
                  in.webhook ? QStringLiteral("delivering to the configured url")
                             : QStringLiteral("no [webhook] url"),
                  QString());
    tiles << tile(QStringLiteral("Telemetry in"),
                  QStringLiteral("Prometheus /metrics"),
                  in.metrics ? QStringLiteral("LIVE") : QStringLiteral("OFF"),
                  in.metrics
                      ? QStringLiteral("scrape endpoint answering")
                      : QStringLiteral("[metrics] endpoint is off"),
                  QString());

    // --- Cloud and fleet ------------------------------------------------
    tiles << tile(QStringLiteral("Cloud & fleet"),
                  QStringLiteral("Cloud identity"),
                  in.cloud ? QStringLiteral("LIVE") : QStringLiteral("MISSING"),
                  in.cloud ? QStringLiteral("%1 instance metadata")
                                 .arg(in.cloudProvider)
                           : QStringLiteral("no instance metadata service"),
                  QStringLiteral("Cloud"));
    tiles << tile(QStringLiteral("Cloud & fleet"), QStringLiteral("Fleet peers"),
                  in.peers > 0 ? QStringLiteral("LIVE")
                               : QStringLiteral("OFF"),
                  in.peers > 0 ? QStringLiteral("%1 peer(s)").arg(in.peers)
                               : QStringLiteral("no [fleet] peers"),
                  QStringLiteral("Fleet"));
    tiles << tile(QStringLiteral("Cloud & fleet"), QStringLiteral("Relay link"),
                  in.relay ? QStringLiteral("LIVE") : QStringLiteral("OFF"),
                  in.relay ? QStringLiteral("reporting to the relay")
                           : QStringLiteral("[remote] is off"),
                  QStringLiteral("Team"));
    tiles << tile(QStringLiteral("Cloud & fleet"),
                  QStringLiteral("Machine labels"),
                  in.labels > 0 ? QStringLiteral("LIVE")
                                : QStringLiteral("READY"),
                  in.labels > 0
                      ? QStringLiteral("%1 label(s)").arg(in.labels)
                      : QStringLiteral("none set - add [fleet] labels.self"),
                  QStringLiteral("Fleet"));
    tiles << tile(QStringLiteral("Cloud & fleet"),
                  QStringLiteral("History ring"),
                  in.history ? QStringLiteral("LIVE") : QStringLiteral("OFF"),
                  in.history ? QStringLiteral("%1 sample(s) on disk")
                                   .arg(in.historySamples)
                             : QStringLiteral("[history] is disabled"),
                  QStringLiteral("History"));

    return tiles;
}

QString integrationSummaryText(const QList<IntegrationTile> &tiles) {
    int live = 0;
    int ready = 0;
    int off = 0;
    int gated = 0;
    for (const IntegrationTile &t : tiles) {
        if (t.state == QStringLiteral("LIVE")) {
            ++live;
        } else if (t.state == QStringLiteral("READY")) {
            ++ready;
        } else if (t.state == QStringLiteral("GATED")) {
            ++gated;
        } else {
            ++off;
        }
    }
    return QStringLiteral("%1 live · %2 ready · %3 off or missing · %4 gated "
                          "on this OS")
        .arg(live)
        .arg(ready)
        .arg(off)
        .arg(gated);
}
