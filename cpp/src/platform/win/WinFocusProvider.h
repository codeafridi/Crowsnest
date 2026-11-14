#pragma once

#include "FocusedWindowProvider.h"

#include <QString>

class WinFocusProvider final : public FocusedWindowProvider {
public:
    WinFocusProvider() = default;
    ~WinFocusProvider() override = default;

    QString name() const override;
    int focusedPid() const override;

private:
    // The overlay never activates (WS_EX_NOACTIVATE), and our own popups are
    // this process. Windows also answers GetForegroundWindow with NULL while
    // it settles after such a click. None of those is a focused app, so the
    // last real one is kept instead of clearing the panel.
    mutable int m_lastPid = -1;
};
