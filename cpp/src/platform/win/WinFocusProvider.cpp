#include "platform/win/WinFocusProvider.h"

#if defined(Q_OS_WIN)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

QString WinFocusProvider::name() const {
    return QStringLiteral("windows-getforegroundwindow");
}

int WinFocusProvider::focusedPid() const {
    const HWND window = ::GetForegroundWindow();
    if (window != nullptr) {
        DWORD pid = 0;
        if (::GetWindowThreadProcessId(window, &pid) != 0 && pid != 0
                && pid != ::GetCurrentProcessId()) {
            m_lastPid = static_cast<int>(pid);
            return m_lastPid;
        }
    }
    // A click on the overlay leaves the foreground unchanged or empty, and
    // any Qt popup it opens belongs to this process; report the app the user
    // was actually in so the focus panels do not blank out.
    return m_lastPid;
}

#endif
