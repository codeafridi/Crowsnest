#include "FocusSampler.h"
#include "FocusBridge.h"
#include "FocusedWindowProvider.h"
#include "GnomePidFileProvider.h"
#include "LinuxProcess.h"
#include "ProcessIdentity.h"
#include "platform/null/NullBackend.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <signal.h>
#if !defined(Q_OS_WIN)
#include <sys/wait.h>
#endif
#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

#include <cstdio>
#if defined(Q_OS_WIN)
int main() {
    std::printf("skip: POSIX-only test\n");
    return 0;
}
#else
#if defined(Q_OS_WIN)
#define GETPID() ::_getpid()
#else
#define GETPID() ::getpid()
#endif

#ifdef Q_OS_LINUX

namespace {

class FakeProvider : public FocusedWindowProvider {
public:
    explicit FakeProvider(int pid) : m_pid(pid) {}

    QString name() const override { return QStringLiteral("fake"); }
    int focusedPid() const override { return m_pid; }
    void setPid(int pid) { m_pid = pid; }

private:
    int m_pid;
};

class FakeIdentityBackend : public NullBackend {
public:
    QHash<int, ProcessIdentity> ids;

    bool exists(int pid) override { return ids.contains(pid); }
    QString comm(int pid) override
    {
        return ids.contains(pid) ? QStringLiteral("app%1").arg(pid)
                                 : QString();
    }
    ProcessIdentity identity(int pid) override { return ids.value(pid); }
};

int spawnSleeper() {
    const pid_t pid = fork();
    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    return static_cast<int>(pid);
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    int failures = 0;
    auto check = [&failures](bool condition, const char *message) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) {
            ++failures;
        }
    };

    const int self = GETPID();
    const int deadPid = 1 << 30;

    check(linuxproc::exists(self), "exists(self) is true");
    check(!linuxproc::exists(deadPid), "exists(bogus pid) is false");
    check(!linuxproc::exists(-1), "exists(-1) is false");
    check(!linuxproc::comm(self).isEmpty(), "comm(self) is not empty");
    check(linuxproc::comm(deadPid).isEmpty(), "comm(bogus pid) is empty");
    check(linuxproc::startTime(self) > 0, "startTime(self) > 0");
    check(linuxproc::identity(self).isValid(), "identity(self) is valid");
    check(!linuxproc::identity(deadPid).isValid(), "identity(bogus pid) invalid");

    QTemporaryFile tmp;
    QString pidFilePath;
    if (tmp.open()) {
        tmp.write(QByteArray::number(self));
        tmp.write("\n");
        tmp.flush();
        pidFilePath = tmp.fileName();
    }

    GnomePidFileProvider fileProvider(pidFilePath);
    check(fileProvider.focusedPid() == self, "pid-file provider resolves live pid");

    GnomePidFileProvider missingProvider(
        QStringLiteral("/nonexistent/crowsnest-active-pid"));
    check(missingProvider.focusedPid() == -1, "missing pid file -> -1");

    GnomePidFileProvider emptyPathProvider{QString()};
    check(emptyPathProvider.focusedPid() == -1, "empty path -> -1");

    QTemporaryDir legacyDir;
    if (legacyDir.isValid()) {
        QFile legacyFile(
            legacyDir.filePath(QStringLiteral("syspeek-active-pid")));
        if (legacyFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            legacyFile.write(QByteArray::number(self));
            legacyFile.write("\n");
            legacyFile.flush();
            GnomePidFileProvider legacyProvider(
                legacyDir.filePath(QStringLiteral("crowsnest-active-pid")));
            check(legacyProvider.focusedPid() == self,
                  "legacy bridge filename still answers");
        } else {
            check(false, "legacy pid file wrote");
        }
    }

    const int live = spawnSleeper();
    check(live > 0 && linuxproc::exists(live), "spawned a live external process");

    FakeProvider liveProvider(live);
    FocusSampler liveSampler(&liveProvider);
    const FocusSample liveSample = liveSampler.sample();
    check(liveSample.available, "sampler available for live pid");
    check(liveSample.identity.pid == live, "sampler reports correct pid");
    check(liveSample.appName == linuxproc::comm(live), "sampler reports comm name");
    check(liveSample.providerName == QStringLiteral("fake"),
          "sampler reports provider name");

    FakeProvider selfProvider(self);
    FocusSampler selfSampler(&selfProvider);
    check(!selfSampler.sample().available,
          "sampler ignores Crowsnest's own pid (no prior app)");

    FakeProvider fallbackProvider(live);
    FocusSampler fallbackSampler(&fallbackProvider);
    check(fallbackSampler.sample().identity.pid == live,
          "fallback baseline is the live app");
    fallbackProvider.setPid(self);
    const FocusSample afterSelf = fallbackSampler.sample();
    check(afterSelf.available && afterSelf.identity.pid == live,
          "overlay focus keeps the last real app");

    FakeProvider deadProvider(deadPid);
    FocusSampler deadSampler(&deadProvider);
    check(!deadSampler.sample().available, "sampler unavailable for dead pid");

    FakeProvider noneProvider(-1);
    FocusSampler noneSampler(&noneProvider);
    check(!noneSampler.sample().available, "sampler unavailable for -1");

    FocusSampler nullSampler(nullptr);
    check(!nullSampler.sample().available, "sampler unavailable without provider");

    // --- pinning: one process stays the subject while focus moves, and the
    // --- pin drops when that pid is gone or recycled.
    FakeIdentityBackend pinBackend;
    pinBackend.ids.insert(100, ProcessIdentity{100, 1000});
    pinBackend.ids.insert(200, ProcessIdentity{200, 2000});
    FakeProvider pinProvider(200);
    FocusSampler pinSampler(&pinProvider, pinBackend);
    pinSampler.pin(100);
    FocusSample pinned = pinSampler.sample();
    check(pinned.available && pinned.pinned && pinned.identity.pid == 100,
          "pin outranks live focus");
    check(pinned.providerName == QStringLiteral("pinned"),
          "pinned sample names its source");
    check(pinned.appName == QStringLiteral("app100"),
          "pinned sample names the process");

    pinBackend.ids[100] = ProcessIdentity{100, 9999};
    FocusSample recycled = pinSampler.sample();
    check(!recycled.pinned && recycled.available
              && recycled.identity.pid == 200,
          "a recycled pid drops the pin and live focus resumes");

    pinSampler.pin(999);
    check(!pinSampler.pinned(), "pinning a dead pid is a no-op");
    pinSampler.pin(100);
    check(pinSampler.pinned(), "a valid pid pins");
    pinSampler.unpin();
    check(!pinSampler.sample().pinned, "unpin returns to live focus");

    FocusSampler pinnedNull(nullptr, pinBackend);
    pinnedNull.pin(100);
    FocusSample orphanPin = pinnedNull.sample();
    check(orphanPin.available && orphanPin.pinned
              && orphanPin.identity.pid == 100,
          "a pin works without a live provider");

    // --- the desktop bridge: parse/enable/copy and the sentence the Settings
    // --- card shows.
    check(parseGnomeExtensionList(QStringLiteral("['a@b', \"c\"]"))
              == (QStringList{QStringLiteral("a@b"), QStringLiteral("c")}),
          "gnome enabled-extensions list parses quotes and spaces");
    check(parseGnomeExtensionList(QStringLiteral("[]")).isEmpty(),
          "an empty gnome extension list parses to nothing");
    check(parseGnomeExtensionList(QStringLiteral("@as []")).isEmpty(),
          "the typed empty gsettings list parses to nothing");
    check(gnomeExtensionListText({QStringLiteral("a@b"), QStringLiteral("c")})
              == QStringLiteral("['a@b', 'c']"),
          "gnome enabled-extensions text round-trips");

    QTemporaryDir bridgeRoot;
    QTemporaryDir bridgeHome;
    QDir().mkpath(bridgeRoot.path()
                  + QStringLiteral("/gnome/crowsnest-focus@codeafridi"));    for (const QString &name : {QStringLiteral("metadata.json"),
                                QStringLiteral("extension.js")}) {
        QFile file(bridgeRoot.path()
                   + QStringLiteral("/gnome/crowsnest-focus@codeafridi/")
                   + name);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("{}\n");
        }
    }
    QStringList bridgeCalls;
    auto bridgeRunner = [&bridgeCalls](const QString &program,
                                       const QStringList &args) {
        bridgeCalls << (program + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
        if (program == QStringLiteral("gsettings") && !args.isEmpty()
            && args.first() == QStringLiteral("get")) {
            return FocusBridgeCommandResult{
                true, QStringLiteral("['other@example']"), QString()};
        }
        return FocusBridgeCommandResult{true, QString(), QString()};
    };
    const QByteArray oldDesktop = qgetenv("XDG_CURRENT_DESKTOP");
    const QByteArray oldSession = qgetenv("XDG_SESSION_TYPE");
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");
    qputenv("XDG_SESSION_TYPE", "wayland");
    const FocusBridgeInstallResult gnomeInstall = focusBridgeInstall(
        bridgeRoot.path(), bridgeHome.path(), bridgeRunner);
    check(gnomeInstall.ok && gnomeInstall.restartRequired,
          "GNOME bridge install succeeds and asks for a relogin");
    check(focusBridgeInstalled(QStringLiteral("gnome"), bridgeHome.path()),
          "GNOME bridge files land in the user's extension dir");
    check(bridgeCalls.join(QLatin1Char('\n'))
              .contains(QStringLiteral("crowsnest-focus@codeafridi")),
          "GNOME install enables the extension");
    check(bridgeCalls.join(QLatin1Char('\n'))
              .contains(QStringLiteral("other@example")),
          "GNOME install keeps the existing enabled extensions");

    const FocusBridgeInstallResult x11Install = [&]() {
        qputenv("XDG_SESSION_TYPE", "x11");
        const FocusBridgeInstallResult result = focusBridgeInstall(
            bridgeRoot.path(), bridgeHome.path(), bridgeRunner);
        qputenv("XDG_SESSION_TYPE", "wayland");
        return result;
    }();
    check(x11Install.ok && x11Install.message.contains(QStringLiteral("X11")),
          "X11 sessions do not install a bridge");

    FocusStatusInput status;
    status.desktop = QStringLiteral("gnome");
    status.sessionType = QStringLiteral("wayland");
    check(focusStatusText(status).contains(QStringLiteral("not installed")),
          "status names a missing GNOME bridge");
    status.bridgeInstalled = true;
    check(focusStatusText(status).contains(QStringLiteral("not loaded")),
          "status distinguishes installed from loaded");
    status.available = true;
    status.provider = QStringLiteral("gnome-pidfile");
    status.app = QStringLiteral("firefox");
    status.pid = QStringLiteral("7");
    check(focusStatusText(status).contains(QStringLiteral("live via")),
          "status names a live provider");
    status.pinned = true;
    check(focusStatusText(status).contains(QStringLiteral("pinned to")),
          "status names a pinned subject");
    FocusStatusInput none;
    check(focusStatusText(none).contains(QStringLiteral("no focus provider")),
          "status is honest without a provider");
    FocusStatusInput x11Status;
    x11Status.provider = QStringLiteral("x11-native");
    check(focusStatusText(x11Status).contains(QStringLiteral("focused X11")),
          "status names an idle X11 session");

    if (oldDesktop.isEmpty()) {
        qunsetenv("XDG_CURRENT_DESKTOP");
    } else {
        qputenv("XDG_CURRENT_DESKTOP", oldDesktop);
    }
    if (oldSession.isEmpty()) {
        qunsetenv("XDG_SESSION_TYPE");
    } else {
        qputenv("XDG_SESSION_TYPE", oldSession);
    }

    // Payload discovery: beside the binary (portable) and usr/bridge
    // (AppImage usr/bin/crowsnest). This is the bug 0.13.6 shipped with:
    // /proc/self/exe was not canonicalised, so appDir was /proc/self.
    QTemporaryDir portableRoot;
    const QString portableApp = portableRoot.path() + QStringLiteral("/app");
    const QString portableBridge =
        portableApp + QStringLiteral("/bridge");
    QDir().mkpath(portableBridge
                  + QStringLiteral("/gnome/crowsnest-focus@codeafridi"));
    QFile portableMeta(portableBridge
                       + QStringLiteral("/gnome/crowsnest-focus@codeafridi/metadata.json"));
    if (portableMeta.open(QIODevice::WriteOnly)) {
        portableMeta.write("{}\n");
        portableMeta.close();
    }
    check(focusBridgePayloadRootIn(portableApp)
              == QFileInfo(portableBridge).canonicalFilePath(),
          "payload is found beside the portable binary");

    QTemporaryDir imageRoot;
    QDir().mkpath(imageRoot.path() + QStringLiteral("/usr/bin"));
    const QString imageBridge =
        imageRoot.path() + QStringLiteral("/usr/bridge");
    QDir().mkpath(imageBridge
                  + QStringLiteral("/gnome/crowsnest-focus@codeafridi"));
    QFile imageMeta(imageBridge
                    + QStringLiteral("/gnome/crowsnest-focus@codeafridi/metadata.json"));
    if (imageMeta.open(QIODevice::WriteOnly)) {
        imageMeta.write("{}\n");
        imageMeta.close();
    }
    check(focusBridgePayloadRootIn(imageRoot.path()
                                   + QStringLiteral("/usr/bin"))
              == QFileInfo(imageBridge).canonicalFilePath(),
          "payload is found above an AppImage binary");

    kill(live, SIGKILL);
    waitpid(live, nullptr, 0);

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(int argc, char *argv[]) {
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    std::printf("focus tests skipped (linux-only)\n");
    return 0;
}

#endif

#endif