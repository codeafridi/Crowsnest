#pragma once

#include "AlertEngine.h"
#include "Config.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// Alert delivery to a user-configured HTTP endpoint. Off by default: Crowsnest
// never sends data anywhere unless the user configures a URL.
//
// Payload shapes are grounded on the shipped Prometheus Alertmanager 0.34.1
// binary and its notifier sources:
//   generic -> Alertmanager webhook v4 message
//     {version:"4", groupKey, truncatedAlerts, receiver, status, alerts:[
//      {status, labels, annotations, startsAt, endsAt, generatorURL,
//       fingerprint}], groupLabels, commonLabels, commonAnnotations,
//      externalURL}
//     (notify/webhook/webhook.go Message + template/template.go Data tags)
//   slack   -> Slack incoming-webhook message {text, attachments:[
//      {fallback, color, title, fields[], mrkdwn_in}]}
//     (notify/slack/slack.go SlackMessage/SlackAttachment/SlackField)
struct WebhookEvent {
    QString id;
    QString title;
    QString detail;
    QString severity;
    QString status; // "firing" | "resolved"
    QString group;  // M87: the fold key the console grouped this alert under
    QString owner;  // M87: the assignment (empty = unowned)
    QString runbook; // what to do about it (empty = not configured)
    qint64 startsAtMs = 0;
    qint64 endsAtMs = 0;
};

// The receiver decides the shape. generic is Alertmanager v4 (works with
// Alertmanager, and with anything that already parses it); slack, discord and
// teams are the chat receivers people actually run at home; ntfy is the
// self-hosted push service; telegram puts the bot token in the URL and the
// chat id in the body.
enum class WebhookFormat {
    Generic,
    Slack,
    Ntfy,
    Telegram,
    Discord,
    Teams,
};

WebhookFormat parseWebhookFormat(const QString &text);
QString webhookFormatName(WebhookFormat format);
QList<QPair<QString, QString>> webhookFormatChoices();

QByteArray alertmanagerWebhookPayload(const QList<WebhookEvent> &events);
QByteArray slackWebhookPayload(const QList<WebhookEvent> &events);
QByteArray webhookPayload(WebhookFormat format,
                          const QList<WebhookEvent> &events,
                          const QString &chatId = QString());
QByteArray webhookContentType(WebhookFormat format);
// Extra request headers a receiver needs (ntfy's Title/Priority/Tags).
QList<QPair<QByteArray, QByteArray>> webhookHeaders(
    WebhookFormat format, const QList<WebhookEvent> &events);
// Telegram carries the bot token in the path and the chat id as a query item;
// it is split out so the id travels in the body and the URL log stays short.
QString webhookPostUrl(WebhookFormat format, const QString &url,
                       QString *chatId);
QString webhookConfigLine(const WebhookConfig &config);

class AlertWebhook : public QObject {
    Q_OBJECT

public:
    explicit AlertWebhook(QObject *parent = nullptr);

    void setConfig(const WebhookConfig &config);
    const WebhookConfig &config() const;

    // Feed the engine's fired transitions plus the currently-active alerts
    // each tick; ids that vanish produce one resolved event each.
    void processEvents(const QList<Alert> &fired, const QList<Alert> &active,
                       qint64 nowMs);

    QString statusLine() const;

    // One synthetic event through the configured receiver, so the operator
    // finds out the token or the URL is wrong before an incident does.
    void sendTest();

signals:
    void deliveryFinished(bool ok, const QString &error);

private:
    void post(const QList<WebhookEvent> &events);
    bool ready() const;

    WebhookConfig m_config;
    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, WebhookEvent> m_active;
    QString m_lastResult;
    bool m_lastOk = true;
    int m_sent = 0;
    int m_failed = 0;
};
