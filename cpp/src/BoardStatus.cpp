#include "BoardStatus.h"

#include <QRegularExpression>
#include <QSet>

namespace {

QString rateText(double bytesPerSec) {
    if (bytesPerSec < 0.0) {
        return QStringLiteral("--");
    }
    const double kb = bytesPerSec / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB/s").arg(qRound(kb));
    }
    const double mb = kb / 1024.0;
    return QStringLiteral("%1 MB/s").arg(mb, 0, 'f', 1);
}

QString bytesText(unsigned long long kb) {
    if (kb == 0) {
        return QStringLiteral("--");
    }
    const double mb = double(kb) / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 0);
    }
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

} // namespace

QList<BoardWidgetInfo> boardCatalog() {
    return {
        {QStringLiteral("app_cpu"), QStringLiteral("App CPU"), QStringLiteral("%")},
        {QStringLiteral("app_mem"), QStringLiteral("App memory"), QStringLiteral("MB")},
        {QStringLiteral("app_net"), QStringLiteral("App network"), QStringLiteral("B/s")},
        {QStringLiteral("host_cpu"), QStringLiteral("Host CPU"), QStringLiteral("%")},
        {QStringLiteral("host_mem"), QStringLiteral("Memory available"), QStringLiteral("GB")},
        {QStringLiteral("host_load"), QStringLiteral("Load 1m"), QString()},
        {QStringLiteral("net_rx"), QStringLiteral("Network in"), QStringLiteral("B/s")},
        {QStringLiteral("net_tx"), QStringLiteral("Network out"), QStringLiteral("B/s")},
        {QStringLiteral("disk_util"), QStringLiteral("Disk utilisation"), QStringLiteral("%")},
        {QStringLiteral("tcp_retrans"), QStringLiteral("TCP retransmits"), QStringLiteral("%")},
        {QStringLiteral("containers"), QStringLiteral("Containers"), QString()},
        {QStringLiteral("kube_pods"), QStringLiteral("Pods"), QString()},
        {QStringLiteral("processes"), QStringLiteral("Processes"), QString()},
    };
}

QString boardWidgetTitle(const QString &id) {
    for (const BoardWidgetInfo &widget : boardCatalog()) {
        if (widget.id == id) {
            return widget.title;
        }
    }
    return QString();
}

QStringList boardParseWidgets(const QString &text) {
    QStringList ids;
    QSet<QString> known;
    for (const BoardWidgetInfo &widget : boardCatalog()) {
        known.insert(widget.id);
    }
    for (QString part : text.split(QRegularExpression(QStringLiteral("[,\\s]+")),
                                   Qt::SkipEmptyParts)) {
        part = part.trimmed().toLower();
        if (part.isEmpty() || !known.contains(part) || ids.contains(part)) {
            continue;
        }
        ids.append(part);
        if (ids.size() >= 12) {
            break;
        }
    }
    return ids;
}

QString boardWidgetsText(const QStringList &ids) {
    return ids.join(QLatin1Char(','));
}

QStringList boardPreset(const QString &name) {
    if (name == QStringLiteral("containers")) {
        return {QStringLiteral("containers"), QStringLiteral("kube_pods"),
                QStringLiteral("host_cpu"), QStringLiteral("host_mem")};
    }
    if (name == QStringLiteral("web")) {
        return {QStringLiteral("app_cpu"), QStringLiteral("app_mem"),
                QStringLiteral("net_rx"), QStringLiteral("net_tx"),
                QStringLiteral("processes")};
    }
    // host, and the fallback for an unknown name
    return {QStringLiteral("host_cpu"), QStringLiteral("host_mem"),
            QStringLiteral("host_load"), QStringLiteral("disk_util")};
}

QStringList boardDefaultWidgets() {
    // A fresh install opens with the board empty: the Overview already has a
    // curated page, and a board nobody chose is furniture.
    return {};
}

QString boardWidgetValue(const QString &id, const BoardInputs &in) {
    if (id == QStringLiteral("app_cpu")) {
        return in.appCpuValid
            ? QStringLiteral("%1%").arg(in.appCpuPercent, 0, 'f', 1)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("app_mem")) {
        return bytesText(in.appRssKb);
    }
    if (id == QStringLiteral("app_net")) {
        if (in.appNetRxPerSec < 0.0 && in.appNetTxPerSec < 0.0) {
            return QStringLiteral("--");
        }
        return rateText(qMax(0.0, in.appNetRxPerSec) + qMax(0.0, in.appNetTxPerSec));
    }
    if (id == QStringLiteral("host_cpu")) {
        return in.hostAvailable
            ? QStringLiteral("%1%").arg(in.hostCpuPercent, 0, 'f', 1)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("host_mem")) {
        if (!in.hostAvailable || in.hostMemAvailableKb == 0) {
            return QStringLiteral("--");
        }
        return QStringLiteral("%1 GB")
            .arg(double(in.hostMemAvailableKb) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (id == QStringLiteral("host_load")) {
        return in.hostAvailable
            ? QStringLiteral("%1").arg(in.hostLoad1, 0, 'f', 2)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("net_rx")) {
        return rateText(in.netRxPerSec);
    }
    if (id == QStringLiteral("net_tx")) {
        return rateText(in.netTxPerSec);
    }
    if (id == QStringLiteral("disk_util")) {
        return in.diskAvailable
            ? QStringLiteral("%1%").arg(in.diskUtilPercent, 0, 'f', 0)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("tcp_retrans")) {
        return in.tcpAvailable
            ? QStringLiteral("%1%").arg(in.tcpRetransPercent, 0, 'f', 2)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("containers")) {
        return in.containersRunning >= 0
            ? QString::number(in.containersRunning)
            : QStringLiteral("--");
    }
    if (id == QStringLiteral("kube_pods")) {
        return in.kubePods >= 0 ? QString::number(in.kubePods)
                                : QStringLiteral("--");
    }
    if (id == QStringLiteral("processes")) {
        return in.processes >= 0 ? QString::number(in.processes)
                                 : QStringLiteral("--");
    }
    return QStringLiteral("--");
}

QString boardWidgetNote(const QString &id, const BoardInputs &in) {
    if (id == QStringLiteral("app_cpu") || id == QStringLiteral("app_mem")
        || id == QStringLiteral("app_net")) {
        return QStringLiteral("focused app");
    }
    if (id == QStringLiteral("containers")) {
        return QStringLiteral("Docker engine");
    }
    if (id == QStringLiteral("kube_pods")) {
        return QStringLiteral("current context");
    }
    if (id == QStringLiteral("tcp_retrans")) {
        return in.tcpAvailable ? QStringLiteral("host counters")
                               : QStringLiteral("gated on this platform");
    }
    return QStringLiteral("this host");
}
