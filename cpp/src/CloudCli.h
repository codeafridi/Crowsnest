#pragma once

#include <QString>
#include <QStringList>

// The provider CLIs (aws, az, gcloud, oci) are user-installed tools. A HUD on a
// macOS LaunchAgent, a Linux systemd user unit, or a shortcut started from a
// graphical session inherits a PATH that is narrower than a login shell's:
// LaunchAgent gets /usr/bin:/bin:/usr/sbin:/sbin, and a systemd unit misses
// ~/.local/bin. Homebrew, snap, the AWS v2 installer and the Google Cloud SDK
// all install outside that PATH, so "not on PATH" was reported for an `aws`
// the user had just installed and verified in their own terminal.
//
// This resolves like a login shell would: PATH first, then the directories the
// four providers actually ship into. CROWSNEST_CLI_DIRS (a PATH-style list)
// prepends explicit directories, for an unusual install or a test that must not
// depend on what the host has installed.
QStringList cloudCliSearchDirs();

// An explicit path is authoritative: when it is set but missing or not
// executable, the result is empty rather than a silent fallthrough to PATH. A
// test or an operator pins the binary that was verified, and a stale pin must
// fail loudly rather than run something else.
QString cloudCliResolve(const QString &cliName,
                        const QString &explicitPath = QString());
