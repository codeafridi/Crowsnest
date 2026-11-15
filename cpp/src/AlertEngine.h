#pragma once

#include <QHash>
#include <QList>
#include <QString>

struct AlertRule {
    QString id;
    QString title;
    QString severity; // "critical" | "warning"
    double threshold = 0.0;
    bool above = true; // breach when value > threshold (else <)
    int sustained = 1; // consecutive breaches required
    QString unit;
    // M87: correlation - group_by key and ownership, the Alertmanager/PagerDuty
    // pair. An empty group folds the rule alone (its own id is the key); a set
    // group folds every rule sharing it onto ONE incident row.
    QString group;   // group_by key
    QString owner;   // assignment; empty renders "unowned"
    // Alertmanager group_wait / repeat_interval: hold the first notification
    // for a new group, then re-notify a still-firing group only this often.
    qint64 groupWaitMs = 5000;
    qint64 repeatIntervalMs = 300000;
    // What to do about it. Carried into the alert row and into every outbound
    // payload, so a pager or a chat message names the page to open.
    QString runbook;
};

struct Alert {
    QString id;
    QString title;
    QString detail;
    QString severity;
    double value = 0.0;
    qint64 atMs = 0;
    // M87: correlation fields carried from the rule, so the outbound webhook
    // payload can label every alert with the group it folds into and who owns
    // it - the receiver groups and routes the same way the console does.
    QString group;
    QString owner;
    QString runbook;
};

// Latest observed values, assembled from the app/host/services/journal snapshots.
struct AlertInput {
    bool appCpuValid = false;
    double appCpu = 0.0;
    bool hostCpuValid = false;
    double hostCpu = 0.0;
    bool hostMemValid = false;
    double hostMemFreePct = 100.0;
    bool diskValid = false;
    double diskFreePct = 100.0;
    bool servicesValid = false;
    int servicesFailed = 0;
    bool k8sValid = false;
    int k8sIssues = 0;
    bool journalValid = false;
    int journalErrors = 0;
    bool watchdogValid = false;
    double watchdogScore = 0.0;        // max robust anomaly score
    int watchdogForecastSteps = 0;     // steps until forecast threshold
    bool kernelFdValid = false;
    double kernelFdPct = 0.0;          // file-nr allocated % of max
    bool stealValid = false;
    double stealPercent = 0.0;
    bool tempValid = false;
    double tempCelsius = 0.0;          // hottest hwmon sensor
    bool batteryValid = false;
    double batteryPercent = 100.0;     // lowest battery charge
    // M88: the account's current spend per hour against its trailing baseline.
    bool cloudBurnValid = false;
    double cloudBurnUsdHour = 0.0;
    // The budget guardrail: month to date, and the month end the provider's
    // forecast implies. Both come from the billed-spend query, so they are
    // only valid once that has answered.
    bool cloudSpendValid = false;
    double cloudSpendMonthUsd = 0.0;
    double cloudSpendProjectedUsd = 0.0;
};

class AlertEngine {
public:
    AlertEngine();

    // Returns alerts that just started firing (call once per cycle).
    QList<Alert> update(const AlertInput &input, qint64 nowMs);

    const QList<Alert> &active() const { return m_active; }
    void reset();

    QList<AlertRule> rules() const { return m_rules; }
    bool setRuleThreshold(const QString &id, double threshold);
    bool setRuleSustained(const QString &id, int sustained);
    // The runbook link shown on the alert row and sent with the payload.
    bool setRuleRunbook(const QString &id, const QString &runbook);
    // M87: the correlated identity of a rule - fold key, owner, and the
    // Alertmanager cadence. Kept separate from the threshold editors so a
    // config load never clobbers a live edit and vice versa.
    bool setRuleCorrelation(const QString &id, const QString &group,
                            const QString &owner, qint64 groupWaitMs,
                            qint64 repeatIntervalMs);
    void resetRules();

private:
    struct State {
        int streak = 0;
        bool firing = false;
    };

    void clearRuleState(const QString &id);

    QList<AlertRule> m_rules;
    QHash<QString, State> m_state;
    QList<Alert> m_active;
};

QList<AlertRule> defaultAlertRules();
