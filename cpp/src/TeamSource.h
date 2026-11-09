#pragma once

#include "TeamStatus.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>

class QTimer;

// M151: the relay's fleet read, on a thread of its own.
//
// Same discipline as the reporter (M97) and the peer poll (M94): a blocking
// socket with a hard budget, never on the sampler's tick, and no thread at all
// unless [remote] enabled=true - so the free build still opens nothing. The
// relay's answer is larger than a peer's summary, so the GET dechunks a
// Transfer-Encoding body (Go streams JSON past its 2 KiB buffer).

struct TeamOptions {
    bool enabled = false;
    bool actions = false;
    QString baseUrl; // normalized, no path
    QString token;
    QString caFile;
    QString selfId;   // this machine's relay id
    QString actor;    // the name an action is posted under
    int pollMs = kTeamDefaultPollMs;
    int timeoutMs = 3000;
};

struct TeamFetchResult {
    bool ok = false;
    int httpStatus = 0;
    QByteArray body;
    QString error; // transport failure or the relay's own detail
};

TeamFetchResult teamHttpGet(const TeamOptions &options, const QString &path);

// One synchronous pass, used by --dump and the tests. Never call it from the
// sampler tick: it blocks for up to timeoutMs.
TeamStatus probeTeam(const TeamOptions &options, qint64 nowMs = 0);

// The link fields of a status with no answer yet, so the page can state the
// switch even when the runner's thread does not exist.
TeamStatus teamStatusFromOptions(const TeamOptions &options);

class TeamRunner : public QObject {
    Q_OBJECT
public:
    explicit TeamRunner(QObject *parent = nullptr);
    ~TeamRunner() override;

    void setOptions(const TeamOptions &options);
    TeamOptions options() const;

public slots:
    void start();
    void stop();
    void pollNow();

signals:
    void teamUpdated(const TeamStatus &status);

private:
    void poll();
    void publish(const TeamStatus &status);

    mutable QMutex m_mutex;
    TeamOptions m_options;
    TeamStatus m_last;
    QTimer *m_timer = nullptr;
};
