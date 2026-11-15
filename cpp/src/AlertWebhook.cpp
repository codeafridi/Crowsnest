#include "QtCompat.h"
#include "AlertWebhook.h"

#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>

namespace {

constexpr int kMaxEventsPerPost = 20;

QString isoTime(qint64 ms) {
    if (ms <= 0) {
        return QStringLiteral("0001-01-01T00:00:00Z");
    }
    return qtcompat::fromMSecsSinceEpochUtc(ms).toString(Qt::ISODate);
}

QString slackColor(const WebhookEvent &event) {
    if (event.status == QLatin1String("resolved")) {
        return QStringLiteral("36a64f");
    }
    const QString severity = event.severity.toLower();
    if (severity == QLatin1String("critical")
        || severity == QLatin1String("high")) {
        return QStringLiteral("d1685f");
    }
    if (severity == QLatin1String("warning")
        || severity == QLatin1String("warn")) {
        return QStringLiteral("d9a04a");
    }
    return QStringLiteral("7f6ff2");
}

WebhookEvent eventFor(const Alert &alert, const QString &status,
                      qint64 startsAtMs, qint64 endsAtMs) {
    WebhookEvent event;
    event.id = alert.id;
    event.title = alert.title;
    event.detail = alert.detail;
    event.severity = alert.severity;
    event.status = status;
    event.group = alert.group.isEmpty() ? alert.id : alert.group;
    event.owner = alert.owner;
    event.runbook = alert.runbook;
    event.startsAtMs = startsAtMs > 0 ? startsAtMs : alert.atMs;
    event.endsAtMs = endsAtMs;
    return event;
}

// Alertmanager's groupKey is the encoded group-label set; a batch that folds
// under one key carries it, a mixed batch falls back to the receiver name.
QString batchGroupKey(const QList<WebhookEvent> &events) {
    if (events.isEmpty()) {
        return QStringLiteral("crowsnest");
    }
    const QString group = events.first().group.trimmed();
    if (group.isEmpty()) {
        return QStringLiteral("crowsnest");
    }
    for (const WebhookEvent &event : events) {
        if (event.group != events.first().group) {
            return QStringLiteral("crowsnest");
        }
    }
    return group;
}

} // namespace

QByteArray alertmanagerWebhookPayload(const QList<WebhookEvent> &events) {
    QJsonArray alerts;
    for (const WebhookEvent &event : events) {
        QJsonObject labels{
            {QStringLiteral("alertname"), event.id},
            {QStringLiteral("severity"), event.severity},
            {QStringLiteral("group"), event.group},
            {QStringLiteral("owner"),
             event.owner.isEmpty() ? QStringLiteral("unowned") : event.owner},
            {QStringLiteral("source"), QStringLiteral("crowsnest")}};
        QJsonObject annotations{
            {QStringLiteral("summary"), event.title},
            {QStringLiteral("description"), event.detail}};
        if (!event.runbook.isEmpty()) {
            annotations.insert(QStringLiteral("runbook"), event.runbook);
        }
        QJsonObject alert{
            {QStringLiteral("status"), event.status},
            {QStringLiteral("labels"), labels},
            {QStringLiteral("annotations"), annotations},
            {QStringLiteral("startsAt"), isoTime(event.startsAtMs)},
            {QStringLiteral("endsAt"), isoTime(event.endsAtMs)},
            {QStringLiteral("generatorURL"), QString()},
            {QStringLiteral("fingerprint"), event.id}};
        alerts.append(alert);
    }

    const QString groupKey = batchGroupKey(events);
    QJsonObject groupLabels;
    groupLabels.insert(QStringLiteral("group"), groupKey);
    QJsonObject commonLabels{
        {QStringLiteral("source"), QStringLiteral("crowsnest")}};
    bool sameOwner = !events.isEmpty();
    for (const WebhookEvent &event : events) {
        if (event.owner != events.first().owner) {
            sameOwner = false;
            break;
        }
    }
    if (sameOwner) {
        commonLabels.insert(QStringLiteral("owner"),
                            events.first().owner.isEmpty()
                                ? QStringLiteral("unowned")
                                : events.first().owner);
    }
    QJsonObject message{
        {QStringLiteral("version"), QStringLiteral("4")},
        {QStringLiteral("groupKey"), groupKey},
        {QStringLiteral("truncatedAlerts"), 0},
        {QStringLiteral("receiver"), QStringLiteral("crowsnest")},
        {QStringLiteral("status"),
         events.isEmpty() ? QString()
                          : events.first().status},
        {QStringLiteral("alerts"), alerts},
        {QStringLiteral("groupLabels"), groupLabels},
        {QStringLiteral("commonLabels"), commonLabels},
        {QStringLiteral("commonAnnotations"), QJsonObject{}},
        {QStringLiteral("externalURL"), QString()}};
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

QByteArray slackWebhookPayload(const QList<WebhookEvent> &events) {
    const bool resolved = !events.isEmpty()
                          && events.first().status
                                 == QLatin1String("resolved");
    const QString text =
        resolved
            ? QStringLiteral(":white_check_mark: Crowsnest resolved %1 alert(s)")
                  .arg(events.size())
            : QStringLiteral(":rotating_light: Crowsnest firing %1 alert(s)")
                  .arg(events.size());

    QJsonArray attachments;
    for (const WebhookEvent &event : events) {
        QJsonArray fields{
            QJsonObject{{QStringLiteral("title"), QStringLiteral("severity")},
                        {QStringLiteral("value"), event.severity},
                        {QStringLiteral("short"), true}},
            QJsonObject{{QStringLiteral("title"), QStringLiteral("status")},
                        {QStringLiteral("value"), event.status},
                        {QStringLiteral("short"), true}},
            QJsonObject{{QStringLiteral("title"), QStringLiteral("owner")},
                        {QStringLiteral("value"),
                         event.owner.isEmpty() ? QStringLiteral("unowned")
                                               : event.owner},
                        {QStringLiteral("short"), true}},
            QJsonObject{{QStringLiteral("title"), QStringLiteral("group")},
                        {QStringLiteral("value"), event.group},
                        {QStringLiteral("short"), true}}};
        if (!event.runbook.isEmpty()) {
            fields.append(QJsonObject{
                {QStringLiteral("title"), QStringLiteral("runbook")},
                {QStringLiteral("value"), event.runbook},
                {QStringLiteral("short"), true}});
        }
        QJsonObject attachment{
            {QStringLiteral("fallback"),
             QStringLiteral("%1 [%2]").arg(event.title, event.status)},
            {QStringLiteral("color"), slackColor(event)},
            {QStringLiteral("title"), event.title},
            {QStringLiteral("text"), event.detail},
            {QStringLiteral("fields"), fields},
            {QStringLiteral("mrkdwn_in"),
             QJsonArray{QStringLiteral("text")}}};
        attachments.append(attachment);
    }

    QJsonObject message{
        {QStringLiteral("text"), text},
        {QStringLiteral("attachments"), attachments}};
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

// --- receiver shapes -------------------------------------------------------
//
// Wire contracts, published by the receivers themselves (Tier B): ntfy takes
// the message as the request body and Title/Priority/Tags as headers; Discord
// takes {"content"}; Telegram takes {"chat_id","text"} with the bot token in
// the path; Teams takes a MessageCard (the same shape the relay's notifier
// sends, so one receiver configuration parses both).

WebhookFormat parseWebhookFormat(const QString &text) {
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("slack")) {
        return WebhookFormat::Slack;
    }
    if (value == QLatin1String("ntfy")) {
        return WebhookFormat::Ntfy;
    }
    if (value == QLatin1String("telegram")) {
        return WebhookFormat::Telegram;
    }
    if (value == QLatin1String("discord")) {
        return WebhookFormat::Discord;
    }
    if (value == QLatin1String("teams")) {
        return WebhookFormat::Teams;
    }
    return WebhookFormat::Generic;
}

QString webhookFormatName(WebhookFormat format) {
    switch (format) {
    case WebhookFormat::Slack:
        return QStringLiteral("slack");
    case WebhookFormat::Ntfy:
        return QStringLiteral("ntfy");
    case WebhookFormat::Telegram:
        return QStringLiteral("telegram");
    case WebhookFormat::Discord:
        return QStringLiteral("discord");
    case WebhookFormat::Teams:
        return QStringLiteral("teams");
    case WebhookFormat::Generic:
        break;
    }
    return QStringLiteral("generic");
}

QList<QPair<QString, QString>> webhookFormatChoices() {
    return {
        {QStringLiteral("generic"),
         QStringLiteral("Alertmanager webhook v4 (works with anything that "
                        "parses it)")},
        {QStringLiteral("slack"), QStringLiteral("Slack incoming webhook")},
        {QStringLiteral("discord"), QStringLiteral("Discord webhook")},
        {QStringLiteral("teams"), QStringLiteral("Microsoft Teams MessageCard")},
        {QStringLiteral("ntfy"),
         QStringLiteral("ntfy topic (https://ntfy.sh/<topic>)")},
        {QStringLiteral("telegram"),
         QStringLiteral("Telegram bot sendMessage (add ?chat_id= to the url)")},
    };
}

QString webhookSeverityWord(const WebhookEvent &event) {
    if (event.status == QLatin1String("resolved")) {
        return QStringLiteral("OK");
    }
    const QString severity = event.severity.toLower();
    if (severity == QLatin1String("critical")
        || severity == QLatin1String("high")) {
        return QStringLiteral("CRIT");
    }
    if (severity == QLatin1String("warning")
        || severity == QLatin1String("warn")) {
        return QStringLiteral("WARN");
    }
    return QStringLiteral("INFO");
}

// One plain-text message, used by every chat receiver. Truncated well inside
// Discord's 2000 character limit; the tail says how many were left out.
QString webhookMessageText(const QList<WebhookEvent> &events) {
    int firing = 0;
    int resolved = 0;
    for (const WebhookEvent &event : events) {
        if (event.status == QLatin1String("resolved")) {
            ++resolved;
        } else {
            ++firing;
        }
    }
    QString head = QStringLiteral("Crowsnest");
    if (firing > 0) {
        head += QStringLiteral(": %1 firing").arg(firing);
    }
    if (resolved > 0) {
        head += QStringLiteral("%1%2 resolved")
                    .arg(firing > 0 ? QStringLiteral(", ") : QStringLiteral(": "))
                    .arg(resolved);
    }
    QStringList lines{head};
    int included = 0;
    for (const WebhookEvent &event : events) {
        QString line = QStringLiteral("[%1] %2 - %3")
                           .arg(webhookSeverityWord(event), event.title,
                                event.detail);
        if (!event.group.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(event.group);
        }
        if (!event.runbook.isEmpty()) {
            line += QStringLiteral("\nrunbook: %1").arg(event.runbook);
        }
        if (lines.join(QLatin1Char('\n')).size() + line.size() > 1800) {
            break;
        }
        lines.append(line);
        ++included;
    }
    if (included < events.size()) {
        lines.append(QStringLiteral("... %1 more")
                         .arg(events.size() - included));
    }
    return lines.join(QLatin1Char('\n'));
}

QByteArray webhookPayload(WebhookFormat format,
                          const QList<WebhookEvent> &events,
                          const QString &chatId) {
    switch (format) {
    case WebhookFormat::Slack:
        return slackWebhookPayload(events);
    case WebhookFormat::Ntfy:
        return webhookMessageText(events).toUtf8();
    case WebhookFormat::Discord:
        return QJsonDocument(QJsonObject{
                                 {QStringLiteral("content"),
                                  webhookMessageText(events)}})
            .toJson(QJsonDocument::Compact);
    case WebhookFormat::Telegram:
        return QJsonDocument(QJsonObject{
                                 {QStringLiteral("chat_id"), chatId},
                                 {QStringLiteral("text"),
                                  webhookMessageText(events)},
                                 {QStringLiteral("disable_web_page_preview"),
                                  true}})
            .toJson(QJsonDocument::Compact);
    case WebhookFormat::Teams: {
        QString color = QStringLiteral("4f9d6f");
        for (const WebhookEvent &event : events) {
            if (event.status == QLatin1String("resolved")) {
                continue;
            }
            const QString severity = event.severity.toLower();
            if (severity == QLatin1String("critical")
                || severity == QLatin1String("high")) {
                color = QStringLiteral("d1685f");
                break;
            }
            color = QStringLiteral("d9a04a");
        }
        QString body = webhookMessageText(events);
        body.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        QJsonObject card{
            {QStringLiteral("@type"), QStringLiteral("MessageCard")},
            {QStringLiteral("@context"),
             QStringLiteral("https://schema.org/extensions")},
            {QStringLiteral("summary"), QStringLiteral("Crowsnest alert")},
            {QStringLiteral("themeColor"), color},
            {QStringLiteral("title"), QStringLiteral("Crowsnest")},
            {QStringLiteral("text"), body}};
        return QJsonDocument(card).toJson(QJsonDocument::Compact);
    }
    case WebhookFormat::Generic:
        break;
    }
    return alertmanagerWebhookPayload(events);
}

QByteArray webhookContentType(WebhookFormat format) {
    if (format == WebhookFormat::Ntfy) {
        return QByteArrayLiteral("text/plain; charset=utf-8");
    }
    return QByteArrayLiteral("application/json");
}

QList<QPair<QByteArray, QByteArray>> webhookHeaders(
    WebhookFormat format, const QList<WebhookEvent> &events) {
    QList<QPair<QByteArray, QByteArray>> headers;
    if (format != WebhookFormat::Ntfy) {
        return headers;
    }
    int firing = 0;
    bool critical = false;
    bool warning = false;
    for (const WebhookEvent &event : events) {
        if (event.status == QLatin1String("resolved")) {
            continue;
        }
        ++firing;
        const QString severity = event.severity.toLower();
        if (severity == QLatin1String("critical")
            || severity == QLatin1String("high")) {
            critical = true;
        } else if (severity == QLatin1String("warning")
                   || severity == QLatin1String("warn")) {
            warning = true;
        }
    }
    headers.append({QByteArrayLiteral("Title"),
                    QStringLiteral("Crowsnest: %1 firing").arg(firing)
                        .toUtf8()});
    headers.append({QByteArrayLiteral("Priority"),
                    QByteArray::number(critical ? 5 : (warning ? 4 : 3))});
    const QByteArray tags = critical ? QByteArrayLiteral("rotating_light")
                            : warning ? QByteArrayLiteral("warning")
                                      : QByteArrayLiteral("white_check_mark");
    headers.append({QByteArrayLiteral("Tags"), tags});
    return headers;
}

QString webhookPostUrl(WebhookFormat format, const QString &url,
                       QString *chatId) {
    if (chatId != nullptr) {
        chatId->clear();
    }
    if (format != WebhookFormat::Telegram) {
        return url;
    }
    QUrl parsed(url);
    QUrlQuery query(parsed);
    if (chatId != nullptr && query.hasQueryItem(QStringLiteral("chat_id"))) {
        *chatId = query.queryItemValue(QStringLiteral("chat_id"));
        query.removeAllQueryItems(QStringLiteral("chat_id"));
    }
    parsed.setQuery(query);
    return parsed.toString();
}

QString webhookConfigLine(const WebhookConfig &config) {
    if (!config.enabled) {
        return QStringLiteral("off ([webhook] enabled=false)");
    }
    if (config.url.isEmpty()) {
        return QStringLiteral("no url ([webhook] url= missing)");
    }
    // A bot token lives in the URL; never print it into a card or a log.
    QString url = config.url;
    if (parseWebhookFormat(config.format) == WebhookFormat::Telegram) {
        url.replace(QRegularExpression(QStringLiteral("bot[^/]+")),
                    QStringLiteral("bot<token>"));
    }
    return QStringLiteral("%1 -> %2")
        .arg(webhookFormatName(parseWebhookFormat(config.format)), url);
}

AlertWebhook::AlertWebhook(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this)) {
}

void AlertWebhook::setConfig(const WebhookConfig &config) {
    m_config = config;
}

const WebhookConfig &AlertWebhook::config() const {
    return m_config;
}

bool AlertWebhook::ready() const {
    return m_config.enabled && !m_config.url.isEmpty();
}

void AlertWebhook::sendTest() {
    if (!ready()) {
        m_lastOk = false;
        m_lastResult = QStringLiteral("test not sent: %1")
                           .arg(webhookConfigLine(m_config));
        emit deliveryFinished(false, m_lastResult);
        return;
    }
    WebhookEvent event;
    event.id = QStringLiteral("crowsnest.test");
    event.title = QStringLiteral("Test notification");
    event.detail = QStringLiteral("sent from the Remote settings card");
    event.severity = QStringLiteral("warning");
    event.status = QStringLiteral("firing");
    event.group = QStringLiteral("crowsnest");
    event.startsAtMs = QDateTime::currentMSecsSinceEpoch();
    post({event});
}

void AlertWebhook::processEvents(const QList<Alert> &fired,
                                 const QList<Alert> &active, qint64 nowMs) {
    QSet<QString> activeIds;
    activeIds.reserve(active.size());
    for (const Alert &alert : active) {
        activeIds.insert(alert.id);
    }

    QList<WebhookEvent> firing;
    QSet<QString> firedIds;
    for (const Alert &alert : fired) {
        firedIds.insert(alert.id);
        const auto known = m_active.constFind(alert.id);
        const qint64 startsAt = known != m_active.constEnd()
                                    ? known->startsAtMs
                                    : alert.atMs;
        const WebhookEvent event =
            eventFor(alert, QStringLiteral("firing"), startsAt, 0);
        m_active.insert(alert.id, event);
        if (ready()) {
            firing.append(event);
        }
    }

    for (const Alert &alert : active) {
        if (!m_active.contains(alert.id) && !firedIds.contains(alert.id)) {
            m_active.insert(alert.id,
                            eventFor(alert, QStringLiteral("firing"),
                                     alert.atMs, 0));
        }
    }

    QList<WebhookEvent> resolved;
    for (auto it = m_active.begin(); it != m_active.end();) {
        if (!activeIds.contains(it.key())) {
            WebhookEvent event = it.value();
            event.status = QStringLiteral("resolved");
            event.endsAtMs = nowMs;
            resolved.append(event);
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }

    if (!ready()) {
        return;
    }
    if (!firing.isEmpty()) {
        post(firing);
    }
    if (!resolved.isEmpty()) {
        post(resolved);
    }
}

void AlertWebhook::post(const QList<WebhookEvent> &events) {
    if (events.size() > kMaxEventsPerPost) {
        post(events.mid(0, kMaxEventsPerPost));
        post(events.mid(kMaxEventsPerPost));
        return;
    }

    const WebhookFormat format = parseWebhookFormat(m_config.format);
    QString chatId;
    const QString postUrl = webhookPostUrl(format, m_config.url, &chatId);
    const QByteArray payload = webhookPayload(format, events, chatId);

    QNetworkRequest request{QUrl(postUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QString::fromLatin1(webhookContentType(format)));
    for (const QPair<QByteArray, QByteArray> &header :
         webhookHeaders(format, events)) {
        request.setRawHeader(header.first, header.second);
    }
    request.setTransferTimeout(m_config.timeoutMs);
    QNetworkReply *reply = m_network->post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, events]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        bool ok = transportOk && status >= 200 && status < 300;
        QString error;
        if (!ok) {
            if (status > 0) {
                error = QStringLiteral("HTTP %1").arg(status);
            } else {
                error = reply->errorString();
            }
        }
        m_lastOk = ok;
        if (ok) {
            ++m_sent;
            m_lastResult =
                QStringLiteral("ok %1 (%2 %3)")
                    .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")),
                         QString::number(events.size()),
                         events.first().status);
            qInfo().noquote() << QStringLiteral("[webhook] delivered")
                              << events.size() << events.first().status;
        } else {
            ++m_failed;
            m_lastResult = QStringLiteral("error %1").arg(error);
            qWarning().noquote() << QStringLiteral("[webhook] delivery failed:")
                                 << error;
        }
        emit deliveryFinished(ok, error);
    });
}

QString AlertWebhook::statusLine() const {
    QString line = webhookConfigLine(m_config);
    if (!ready()) {
        return line;
    }
    line += QStringLiteral(" - %1 sent, %2 failed").arg(m_sent).arg(m_failed);
    if (!m_lastResult.isEmpty()) {
        line += QStringLiteral("; last ") + m_lastResult;
    }
    return line;
}
