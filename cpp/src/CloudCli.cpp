#include "CloudCli.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace {

bool isRunnable(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
#if defined(Q_OS_WIN)
    return true;
#else
    return info.isExecutable();
#endif
}

QString fromDir(const QString &dir, const QString &cliName) {
    if (dir.isEmpty()) {
        return QString();
    }
    const QString base = QDir(dir).filePath(cliName);
    if (isRunnable(base)) {
        return QFileInfo(base).absoluteFilePath();
    }
#if defined(Q_OS_WIN)
    for (const char *extension : {"exe", "cmd", "bat"}) {
        const QString candidate =
            base + QLatin1Char('.') + QLatin1String(extension);
        if (isRunnable(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
#endif
    return QString();
}

void addDir(QStringList &dirs, const QString &dir) {
    if (!dir.isEmpty() && !dirs.contains(dir)) {
        dirs.append(dir);
    }
}

} // namespace

QStringList cloudCliSearchDirs() {
    QStringList dirs;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // An operator (or a test) can name the directories explicitly. Listed
    // first so it wins over the conventional install locations below.
#if defined(Q_OS_WIN)
    const QStringList extra =
        env.value(QStringLiteral("CROWSNEST_CLI_DIRS")).split(
            QLatin1Char(';'), Qt::SkipEmptyParts);
#else
    const QString home = env.value(QStringLiteral("HOME"));
    const QStringList extra =
        env.value(QStringLiteral("CROWSNEST_CLI_DIRS")).split(
            QLatin1Char(':'), Qt::SkipEmptyParts);
#endif
    for (const QString &dir : extra) {
        addDir(dirs, dir.trimmed());
    }
#if defined(Q_OS_WIN)
    const QString localAppData =
        env.value(QStringLiteral("LOCALAPPDATA"));
    const QString programFiles = env.value(QStringLiteral("ProgramFiles"));
    const QString programFilesX86 =
        env.value(QStringLiteral("ProgramFiles(x86)"));
    if (!localAppData.isEmpty()) {
        addDir(dirs,
               localAppData + QStringLiteral("\\Programs\\Amazon\\AWSCLIV2"));
        addDir(dirs, localAppData + QStringLiteral("\\Microsoft\\WindowsApps"));
        addDir(dirs, localAppData
                        + QStringLiteral("\\Google\\Cloud SDK\\"
                                         "google-cloud-sdk\\bin"));
        addDir(dirs, localAppData
                        + QStringLiteral("\\Programs\\Microsoft SDKs\\Azure"));
    }
    if (!programFiles.isEmpty()) {
        addDir(dirs, programFiles + QStringLiteral("\\Amazon\\AWSCLIV2"));
        addDir(dirs, programFiles
                        + QStringLiteral("\\Google\\Cloud SDK\\"
                                         "google-cloud-sdk\\bin"));
        // az CLI 2 installs its entry point under wbin.
        addDir(dirs, programFiles
                        + QStringLiteral("\\Microsoft SDKs\\Azure\\CLI2\\wbin"));
        addDir(dirs, programFiles
                        + QStringLiteral("\\Microsoft SDKs\\Azure\\CLI2\\bin"));
    }
    if (!programFilesX86.isEmpty()) {
        addDir(dirs, programFilesX86 + QStringLiteral("\\Amazon\\AWSCLIV2"));
    }
#else
    addDir(dirs, QStringLiteral("/usr/local/bin"));
    addDir(dirs, QStringLiteral("/opt/homebrew/bin"));
    addDir(dirs, QStringLiteral("/home/linuxbrew/.linuxbrew/bin"));
    addDir(dirs, QStringLiteral("/usr/local/aws-cli/v2/current/bin"));
    addDir(dirs, QStringLiteral("/usr/lib/google-cloud-sdk/bin"));
    addDir(dirs, QStringLiteral("/snap/bin"));
    addDir(dirs, QStringLiteral("/opt/local/bin"));
    addDir(dirs, QStringLiteral("/usr/local/sbin"));
    if (!home.isEmpty()) {
        addDir(dirs, home + QStringLiteral("/.local/bin"));
        addDir(dirs, home + QStringLiteral("/.linuxbrew/bin"));
        addDir(dirs, home + QStringLiteral("/bin"));
        addDir(dirs, home + QStringLiteral("/google-cloud-sdk/bin"));
        addDir(dirs, home + QStringLiteral("/.local/aws-cli/v2/current/bin"));
        addDir(dirs, home + QStringLiteral("/.azure/bin"));
    }
#endif
    return dirs;
}

QString cloudCliResolve(const QString &cliName, const QString &explicitPath) {
    const QString trimmedName = cliName.trimmed();
    if (trimmedName.isEmpty()) {
        return QString();
    }
    const QString pinned = explicitPath.trimmed();
    if (!pinned.isEmpty()) {
        if (isRunnable(pinned)) {
            return QFileInfo(pinned).absoluteFilePath();
        }
        return QString();
    }
    const QString onPath = QStandardPaths::findExecutable(trimmedName);
    if (!onPath.isEmpty()) {
        return QFileInfo(onPath).absoluteFilePath();
    }
#if defined(Q_OS_WIN)
    for (const char *extension : {"exe", "cmd", "bat"}) {
        const QString candidate = QStandardPaths::findExecutable(
            trimmedName + QLatin1Char('.') + QLatin1String(extension));
        if (!candidate.isEmpty()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
#endif
    const QStringList dirs = cloudCliSearchDirs();
    for (const QString &dir : dirs) {
        const QString candidate = fromDir(dir, trimmedName);
        if (!candidate.isEmpty()) {
            return candidate;
        }
    }
    return QString();
}
