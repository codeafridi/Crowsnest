#include "FocusBridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace {

constexpr const char *kGnomeUuid = "crowsnest-focus@codeafridi";
constexpr const char *kKdeId = "crowsnest-focus";

FocusBridgeCommandResult runCommand(const QString &program,
                                    const QStringList &args) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(2000)) {
        return {false, QString(), process.errorString()};
    }
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(1000);
        return {false, QString(), QStringLiteral("timed out")};
    }
    const bool ok = process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
    return {ok, QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
            QString::fromUtf8(process.readAllStandardError()).trimmed()};
}

bool copyTree(const QString &source, const QString &destination,
              QString *error) {
    QDir sourceDir(source);
    if (!sourceDir.exists()) {
        *error = QStringLiteral("source does not exist: %1").arg(source);
        return false;
    }
    QDir(destination).removeRecursively();
    if (!QDir().mkpath(destination)) {
        *error = QStringLiteral("cannot create %1").arg(destination);
        return false;
    }
    QDirIterator it(source, QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = sourceDir.relativeFilePath(file);
        const QString target = destination + QLatin1Char('/') + relative;
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            *error = QStringLiteral("cannot create %1")
                         .arg(QFileInfo(target).absolutePath());
            return false;
        }
        if (!QFile::copy(file, target)) {
            *error = QStringLiteral("cannot copy %1").arg(relative);
            return false;
        }
    }
    return true;
}

QString firstExisting(const QStringList &programs) {
    for (const QString &program : programs) {
        if (!QStandardPaths::findExecutable(program).isEmpty()) {
            return program;
        }
    }
    return QString();
}

} // namespace

QString focusDesktopId() {
    const QString desktop =
        qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (desktop.contains(QStringLiteral("gnome"))
        || desktop.contains(QStringLiteral("unity"))) {
        return QStringLiteral("gnome");
    }
    if (desktop.contains(QStringLiteral("kde"))
        || desktop.contains(QStringLiteral("plasma"))) {
        return QStringLiteral("kde");
    }
    return QString();
}

QString focusBridgeUserDir(const QString &desktop, const QString &home) {
    const QString base = home.isEmpty() ? QDir::homePath() : home;
    if (desktop == QStringLiteral("gnome")) {
        return base + QStringLiteral("/.local/share/gnome-shell/extensions/")
            + QString::fromLatin1(kGnomeUuid);
    }
    if (desktop == QStringLiteral("kde")) {
        return base + QStringLiteral("/.local/share/kwin/scripts/")
            + QString::fromLatin1(kKdeId);
    }
    return QString();
}

bool focusBridgeInstalled(const QString &desktop, const QString &home) {
    const QString dir = focusBridgeUserDir(desktop, home);
    if (dir.isEmpty()) {
        return false;
    }
    if (!QFileInfo::exists(dir + QStringLiteral("/metadata.json"))) {
        return false;
    }
    if (desktop == QStringLiteral("gnome")) {
        return QFileInfo::exists(dir + QStringLiteral("/extension.js"));
    }
    return QFileInfo::exists(
        dir + QStringLiteral("/contents/code/main.js"));
}

QString focusBridgePayloadRoot() {
    QStringList candidates;
    const QString override = qEnvironmentVariable("CROWSNEST_BRIDGE_DIR");
    if (!override.isEmpty()) {
        candidates << override;
    }
    QString appDir;
    if (QCoreApplication::instance() != nullptr) {
        appDir = QCoreApplication::applicationDirPath();
    }
#if defined(Q_OS_LINUX)
    if (appDir.isEmpty()) {
        // --dump/focus-bridge run before QApplication; /proc/self/exe is the
        // installed binary (and the extracted path inside an AppImage).
        appDir = QFileInfo(QStringLiteral("/proc/self/exe")).absolutePath();
    }
#endif
    if (!appDir.isEmpty()) {
        candidates << appDir + QStringLiteral("/bridge");
        // An AppImage puts the binary at usr/bin/crowsnest and the payload at
        // usr/bridge.
        candidates << appDir + QStringLiteral("/../bridge");
    }
    candidates << QStringLiteral("/usr/share/crowsnest/bridge");
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate
                              + QStringLiteral("/gnome/")
                              + QString::fromLatin1(kGnomeUuid)
                              + QStringLiteral("/metadata.json"))
            || QFileInfo::exists(candidate + QStringLiteral("/kwin/")
                                 + QString::fromLatin1(kKdeId)
                                 + QStringLiteral("/metadata.json"))) {
            return candidate;
        }
    }
    return QString();
}

FocusBridgeInstallResult focusBridgeInstall(const QString &payloadRoot,
                                            const QString &home,
                                            FocusBridgeRunner runner) {
    FocusBridgeInstallResult result;
    const QString desktop = focusDesktopId();
    if (desktop.isEmpty()) {
        result.ok = true;
        result.message =
            QStringLiteral("no desktop focus bridge is used on this session");
        return result;
    }
    if (qEnvironmentVariable("XDG_SESSION_TYPE").compare(
            QStringLiteral("wayland"), Qt::CaseInsensitive) != 0) {
        result.ok = true;
        result.message = QStringLiteral(
            "this is an X11 session - the native X11 provider is used, no "
            "bridge is needed");
        return result;
    }

    const QString root = payloadRoot.isEmpty() ? focusBridgePayloadRoot()
                                               : payloadRoot;
    const QString source = desktop == QStringLiteral("gnome")
        ? root + QStringLiteral("/gnome/") + QString::fromLatin1(kGnomeUuid)
        : root + QStringLiteral("/kwin/") + QString::fromLatin1(kKdeId);
    if (!QFileInfo::exists(source + QStringLiteral("/metadata.json"))) {
        result.message = QStringLiteral(
            "focus bridge payload not found at %1 (reinstall Crowsnest or set "
            "CROWSNEST_BRIDGE_DIR)").arg(source);
        return result;
    }

    const QString target = focusBridgeUserDir(desktop, home);
    QString error;
    if (!copyTree(source, target, &error)) {
        result.message =
            QStringLiteral("could not install the focus bridge: %1").arg(error);
        return result;
    }

    FocusBridgeRunner run = runner ? runner : runCommand;
    QStringList notes;
    if (desktop == QStringLiteral("gnome")) {
        const QString uuid = QString::fromLatin1(kGnomeUuid);
        // gsettings is the authoritative enabled list; gnome-extensions
        // enable is a convenience that can fail before the shell has seen the
        // new directory (it needs a relogin anyway).
        const FocusBridgeCommandResult current = run(
            QStringLiteral("gsettings"),
            {QStringLiteral("get"), QStringLiteral("org.gnome.shell"),
             QStringLiteral("enabled-extensions")});
        if (current.ok) {
            QStringList enabled = parseGnomeExtensionList(current.out);
            if (!enabled.contains(uuid)) {
                enabled << uuid;
                const FocusBridgeCommandResult set = run(
                    QStringLiteral("gsettings"),
                    {QStringLiteral("set"), QStringLiteral("org.gnome.shell"),
                     QStringLiteral("enabled-extensions"),
                     gnomeExtensionListText(enabled)});
                if (!set.ok) {
                    notes << QStringLiteral(
                        "could not add the extension to enabled-extensions: %1")
                                 .arg(set.error);
                }
            }
        } else {
            notes << QStringLiteral(
                "gsettings could not be read (%1); enable the extension in "
                "GNOME Extensions").arg(current.error);
        }
        run(QStringLiteral("gnome-extensions"),
            {QStringLiteral("enable"), uuid});
        result.restartRequired = true;
        result.ok = true;
        result.message =
            QStringLiteral("GNOME focus bridge installed - log out and back "
                           "in to load it");
    } else {
        const QString packageTool = firstExisting(
            {QStringLiteral("kpackagetool6"), QStringLiteral("kpackagetool5")});
        if (!packageTool.isEmpty()) {
            if (!run(packageTool,
                     {QStringLiteral("--type=KWin/Script"),
                      QStringLiteral("-i"), target}).ok) {
                notes << QStringLiteral("KWin package install returned an "
                                        "error (the script may already be "
                                        "installed)");
            }
        } else {
            notes << QStringLiteral("kpackagetool6/5 not found");
        }
        const QString kwrite = firstExisting(
            {QStringLiteral("kwriteconfig6"), QStringLiteral("kwriteconfig5")});
        if (!kwrite.isEmpty()) {
            run(kwrite, {QStringLiteral("--file"), QStringLiteral("kwinrc"),
                         QStringLiteral("--group"), QStringLiteral("Plugins"),
                         QStringLiteral("--key"),
                         QString::fromLatin1(kKdeId) + QStringLiteral("Enabled"),
                         QStringLiteral("true")});
        } else {
            notes << QStringLiteral("kwriteconfig6/5 not found");
        }
        const QString qdbus = firstExisting(
            {QStringLiteral("qdbus6"), QStringLiteral("qdbus")});
        const bool reconfigured = !qdbus.isEmpty()
            && run(qdbus, {QStringLiteral("org.kde.KWin"),
                           QStringLiteral("/KWin"),
                           QStringLiteral("reconfigure")}).ok;
        if (!reconfigured) {
            result.restartRequired = true;
            notes << QStringLiteral("KWin reconfigure failed - log out and "
                                    "back in");
        }
        result.ok = true;
        result.message = QStringLiteral("KDE focus bridge installed");
    }

    if (!notes.isEmpty()) {
        result.message += QStringLiteral(" (%1)").arg(notes.join(QStringLiteral("; ")));
    }
    return result;
}

QString focusStatusText(const FocusStatusInput &input) {
    if (input.pinned) {
        return QStringLiteral("focus  pinned to %1 (pid %2) - UNPIN to follow "
                              "focus again")
            .arg(input.app, input.pid);
    }
    if (input.available) {
        return QStringLiteral("focus  live via %1 - %2 (pid %3)")
            .arg(input.provider, input.app, input.pid);
    }
    if (input.desktop == QStringLiteral("gnome")
        || input.desktop == QStringLiteral("kde")) {
        const bool wayland = input.sessionType.compare(
                                 QStringLiteral("wayland"),
                                 Qt::CaseInsensitive) == 0;
        if (!wayland) {
            if (input.provider == QStringLiteral("x11-native")) {
                return QStringLiteral("focus  off - no focused X11 window");
            }
            return QStringLiteral(
                "focus  off - no focus provider for this X11 session");
        }
    }
    if (input.desktop == QStringLiteral("gnome")) {
        if (!input.bridgeInstalled) {
            return QStringLiteral(
                "focus  off - GNOME bridge not installed (run: crowsnest "
                "focus-bridge)");
        }
        return QStringLiteral(
            "focus  off - GNOME bridge installed but not loaded (run: "
            "crowsnest focus-bridge, then log out and back in)");
    }
    if (input.desktop == QStringLiteral("kde")) {
        if (!input.bridgeInstalled) {
            return QStringLiteral(
                "focus  off - KDE bridge not installed (run: crowsnest "
                "focus-bridge)");
        }
        return QStringLiteral(
            "focus  off - KDE bridge installed but no window reported (check "
            "the KWin script is enabled)");
    }
    if (input.provider == QStringLiteral("x11-native")) {
        return QStringLiteral("focus  off - no focused X11 window");
    }
    if (input.provider == QStringLiteral("mac-nsworkspace")
        || input.provider == QStringLiteral("windows-getforegroundwindow")) {
        return QStringLiteral("focus  off - no foreground window");
    }
    if (input.provider == QStringLiteral("hyprctl")
        || input.provider == QStringLiteral("swaymsg")
        || input.provider == QStringLiteral("niri")) {
        return QStringLiteral(
            "focus  off - the compositor reports no focused window");
    }
    return QStringLiteral("focus  off - no focus provider for this session");
}

QStringList parseGnomeExtensionList(const QString &raw) {
    QString text = raw.trimmed();
    // Some gsettings versions print the type annotation for an empty array
    // (`@as []`) rather than `[]`.
    if (text.startsWith(QStringLiteral("@as"))) {
        text.remove(0, 3);
        text = text.trimmed();
    }
    if (text.startsWith(QLatin1Char('['))) {
        text.remove(0, 1);
    }
    if (text.endsWith(QLatin1Char(']'))) {
        text.chop(1);
    }
    QStringList out;
    for (QString part : text.split(QLatin1Char(','))) {
        part = part.trimmed();
        if (part.size() >= 2
            && ((part.startsWith(QLatin1Char('\''))
                 && part.endsWith(QLatin1Char('\'')))
                || (part.startsWith(QLatin1Char('"'))
                    && part.endsWith(QLatin1Char('"'))))) {
            part = part.mid(1, part.size() - 2);
        }
        if (!part.isEmpty()) {
            out << part;
        }
    }
    return out;
}

QString gnomeExtensionListText(const QStringList &extensions) {
    QStringList quoted;
    for (const QString &extension : extensions) {
        quoted << QStringLiteral("'%1'").arg(extension);
    }
    return QLatin1Char('[') + quoted.join(QStringLiteral(", "))
        + QLatin1Char(']');
}
