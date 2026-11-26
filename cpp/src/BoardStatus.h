#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// A user-composed board: the widgets somebody pins to the Overview because
// they are the numbers they came to see. Datadog's dashboards are a grid of
// widgets over a query API; this console has no query language, so the widget
// catalog is the honest equivalent - a fixed set of measured values, chosen
// and ordered by the user, persisted in the config.
struct BoardWidgetInfo {
    QString id;
    QString title;
    QString unit;
};

struct BoardInputs {
    bool appCpuValid = false;
    double appCpuPercent = 0.0;
    unsigned long long appRssKb = 0;
    double appNetRxPerSec = -1.0;
    double appNetTxPerSec = -1.0;
    bool hostAvailable = false;
    double hostCpuPercent = 0.0;
    unsigned long long hostMemAvailableKb = 0;
    double hostLoad1 = 0.0;
    double netRxPerSec = 0.0;
    double netTxPerSec = 0.0;
    double diskUtilPercent = 0.0;
    bool diskAvailable = false;
    bool tcpAvailable = false;
    double tcpRetransPercent = 0.0;
    int containersRunning = -1;
    int kubePods = -1;
    int processes = -1;
};

QList<BoardWidgetInfo> boardCatalog();
QString boardWidgetTitle(const QString &id);
QStringList boardParseWidgets(const QString &text);
QString boardWidgetsText(const QStringList &ids);
QStringList boardPreset(const QString &name);
QStringList boardDefaultWidgets();
QString boardWidgetValue(const QString &id, const BoardInputs &inputs);
QString boardWidgetNote(const QString &id, const BoardInputs &inputs);
