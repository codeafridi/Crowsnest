#pragma once

#include "ProcessIdentity.h"
#include "platform/PlatformFactory.h"

#include <QString>

class FocusedWindowProvider;

struct FocusSample {
    bool available = false;
    bool pinned = false;
    ProcessIdentity identity;
    QString appName = QStringLiteral("--");
    QString providerName;
};

class FocusSampler {
public:
    explicit FocusSampler(const FocusedWindowProvider *provider,
                          SystemBackend &backend = systemBackend());

    FocusSample sample() const;

    // Pin one process as the subject. The identity is resolved once and
    // re-checked every tick, so a recycled pid cannot inherit the pin. An
    // exited process clears the pin and live focus resumes.
    void pin(int pid);
    void unpin();
    bool pinned() const { return m_pinActive; }

private:
    const FocusedWindowProvider *m_provider;
    SystemBackend &m_backend;
    mutable ProcessIdentity m_lastIdentity;
    mutable QString m_lastName;
    mutable bool m_pinActive = false;
    mutable ProcessIdentity m_pinIdentity;
    mutable QString m_pinName;
};
