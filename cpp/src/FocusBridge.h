#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// The desktop focus bridge is what lets a Wayland session name the focused
// window: a GNOME Shell extension writes the pid, a KWin script sends it over
// D-Bus. This module copies the bundled payload into the user's session,
// enables it, and words the state for Settings/--dump. It never talks to the
// network and never needs root.

struct FocusBridgeCommandResult {
    bool ok = false;
    QString out;
    QString error;
};

using FocusBridgeRunner =
    std::function<FocusBridgeCommandResult(const QString &program,
                                           const QStringList &args)>;

struct FocusBridgeInstallResult {
    bool ok = false;
    bool restartRequired = false;
    QString message;
};

struct FocusStatusInput {
    QString desktop;   // gnome | kde | empty
    QString sessionType;
    QString provider;  // snapshot.focusProvider
    bool available = false;
    bool pinned = false;
    QString app;
    QString pid;
    bool bridgeInstalled = false;
};

// "gnome", "kde" or empty; from XDG_CURRENT_DESKTOP.
QString focusDesktopId();

// The extension/script directory under the user's home.
QString focusBridgeUserDir(const QString &desktop,
                           const QString &home = QString());

// True when both payload files are present in the user's session.
bool focusBridgeInstalled(const QString &desktop,
                          const QString &home = QString());

// Where the running binary keeps the bundled payload: <app>/bridge or
// /usr/share/crowsnest/bridge. Empty when neither exists.
QString focusBridgePayloadRoot();

// Copy + enable. `payloadRoot` defaults to focusBridgePayloadRoot(); `home`
// defaults to QDir::homePath(). The runner is injectable for tests.
FocusBridgeInstallResult focusBridgeInstall(const QString &payloadRoot = QString(),
                                            const QString &home = QString(),
                                            FocusBridgeRunner runner = {});

// The one-line Settings/--dump sentence. Names the next action instead of
// leaving a dash.
QString focusStatusText(const FocusStatusInput &input);

// gsettings text helpers, pure and tested.
QStringList parseGnomeExtensionList(const QString &raw);
QString gnomeExtensionListText(const QStringList &extensions);
