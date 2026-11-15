#include "CloudFleetSource.h"

#include "CloudCli.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

// The account does not change often; a 10 min listing keeps the $/MO figure
// honest without paying for a CLI call on every tick. Instance age is
// recomputed from the cached launch time each tick, so the AGE tile stays live.
constexpr qint64 kCloudFleetCadenceMs = 600000;
constexpr int kCloudFleetRowCap = 200;

constexpr int kCloudFleetStartMs = 5000;
constexpr int kCloudFleetKillGraceMs = 500;
constexpr int kCloudFleetPollSliceMs = 25;
constexpr int kCloudFleetRegionConcurrencyMax = 8;

QString cloudFirstLine(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    return trimmed.section(QLatin1Char('\n'), 0, 0).trimmed();
}

QString cloudQuoteArgument(const QString &argument) {
    if (!argument.isEmpty() && !argument.contains(QLatin1Char(' ')) &&
        !argument.contains(QLatin1Char('\t')) &&
        !argument.contains(QLatin1Char('"'))) {
        return argument;
    }
    QString escaped = argument;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

// Shared by the blocking and the polled path: the process is started the same
// way, so a Windows .cmd shim and a POSIX script both work.
void cloudStartProcess(QProcess *process, const QString &cliName,
                       const QString &cliPath,
                       const QStringList &arguments) {
    process->setProcessChannelMode(QProcess::SeparateChannels);
    const QString suffix = QFileInfo(cliPath).suffix().toLower();
    if (suffix == QLatin1String("cmd") || suffix == QLatin1String("bat")) {
#if defined(Q_OS_WIN)
        process->setProgram(QStringLiteral("cmd.exe"));
        process->setNativeArguments(cloudWindowsCmdLine(cliPath, arguments));
#else
        process->setProgram(cliPath);
        process->setArguments(arguments);
#endif
    } else {
        process->setProgram(cliPath);
        process->setArguments(arguments);
    }
    Q_UNUSED(cliName);
    process->start();
}

enum class FleetPhase {
    Idle,
    Identity,
    HomeRegion,
    Regions,
    Instances,
    // One describe-images per region with rows, plus one describe-instance-types
    // for the types seen: the OS name and the vCPU/memory behind the type.
    Details,
    Done,
};

struct FleetJob {
    QProcess *process = nullptr;
    QString stage;
    QString region;
    // The session's own clock: the logical `nowMs` a caller passes is used for
    // the status timestamps (and tests pin it), while budgets must be measured
    // against real elapsed time or a pinned clock trips them instantly.
    qint64 wallStartedAtMs = 0;
};

} // namespace

QString cloudFleetCli(CloudProvider provider) {
    switch (provider) {
    case CloudProvider::Aws:
        return QStringLiteral("aws");
    case CloudProvider::Azure:
        return QStringLiteral("az");
    case CloudProvider::Gcp:
        return QStringLiteral("gcloud");
    case CloudProvider::Oci:
        return QStringLiteral("oci");
    case CloudProvider::None:
        break;
    }
    return QString();
}

QStringList cloudFleetArguments(const CloudIdentity &identity) {
    switch (identity.provider) {
    case CloudProvider::Aws:
        return cloudAwsDescribeInstancesArguments(identity.region);
    case CloudProvider::Azure: {
        QStringList args;
        args << QStringLiteral("vm") << QStringLiteral("list")
             << QStringLiteral("-d") << QStringLiteral("-o")
             << QStringLiteral("json");
        if (!identity.accountId.isEmpty()) {
            args << QStringLiteral("--subscription") << identity.accountId;
        }
        return args;
    }
    case CloudProvider::Gcp: {
        QStringList args;
        args << QStringLiteral("compute") << QStringLiteral("instances")
             << QStringLiteral("list") << QStringLiteral("--format=json");
        if (!identity.accountId.isEmpty()) {
            args << QStringLiteral("--project") << identity.accountId;
        }
        return args;
    }
    case CloudProvider::Oci: {
        QStringList args;
        args << QStringLiteral("compute") << QStringLiteral("instance")
             << QStringLiteral("list");
        if (!identity.accountId.isEmpty()) {
            args << QStringLiteral("--compartment-id") << identity.accountId;
        }
        if (!identity.region.isEmpty()) {
            args << QStringLiteral("--region") << identity.region;
        }
        args << QStringLiteral("--all") << QStringLiteral("--output")
             << QStringLiteral("json");
        return args;
    }
    case CloudProvider::None:
        break;
    }
    return QStringList();
}

QStringList cloudAwsIdentityArguments() {
    return {QStringLiteral("sts"), QStringLiteral("get-caller-identity"),
            QStringLiteral("--output"), QStringLiteral("json")};
}

QStringList cloudAwsRegionArguments() {
    return {QStringLiteral("configure"), QStringLiteral("get"),
            QStringLiteral("region")};
}

QStringList cloudAwsDescribeRegionsArguments(const QString &homeRegion) {
    QStringList args;
    args << QStringLiteral("ec2") << QStringLiteral("describe-regions")
         << QStringLiteral("--output") << QStringLiteral("json");
    if (!homeRegion.isEmpty()) {
        args << QStringLiteral("--region") << homeRegion;
    }
    return args;
}

QStringList cloudAwsDescribeInstancesArguments(const QString &region) {
    QStringList args;
    args << QStringLiteral("ec2") << QStringLiteral("describe-instances")
         << QStringLiteral("--output") << QStringLiteral("json");
    // The EC2 API is regional and an instance-profile host carries no region
    // in ~/.aws/config, so the region is always passed explicitly.
    if (!region.isEmpty()) {
        args << QStringLiteral("--region") << region;
    }
    // Projecting to the instances keeps the payload to the fields the table
    // renders; the projected members keep the AWS key names, so the parser
    // handles this and the full document the same way.
    args << QStringLiteral("--query")
         << QStringLiteral("Reservations[].Instances[]");
    return args;
}

QStringList cloudAwsDescribeImagesArguments(const QString &region,
                                            const QStringList &imageIds) {
    QStringList args;
    args << QStringLiteral("ec2") << QStringLiteral("describe-images")
         << QStringLiteral("--output") << QStringLiteral("json");
    if (!region.isEmpty()) {
        args << QStringLiteral("--region") << region;
    }
    args << QStringLiteral("--image-ids");
    for (const QString &id : imageIds) {
        args << id;
    }
    args << QStringLiteral("--query")
         << QStringLiteral("Images[].{id:ImageId,name:Name,desc:Description}");
    return args;
}

QStringList cloudAwsDescribeInstanceTypesArguments(const QString &region,
                                                   const QStringList &types) {
    QStringList args;
    args << QStringLiteral("ec2") << QStringLiteral("describe-instance-types")
         << QStringLiteral("--output") << QStringLiteral("json");
    if (!region.isEmpty()) {
        args << QStringLiteral("--region") << region;
    }
    args << QStringLiteral("--instance-types");
    for (const QString &type : types) {
        args << type;
    }
    args << QStringLiteral("--query")
         << QStringLiteral("InstanceTypes[].{type:InstanceType,"
                           "vcpu:VCpuInfo.DefaultVCpus,"
                           "mem:MemoryInfo.SizeInMiB}");
    return args;
}

QStringList cloudAwsDescribeVolumesArguments(const QString &region,
                                             const QStringList &volumeIds) {
    QStringList args;
    args << QStringLiteral("ec2") << QStringLiteral("describe-volumes")
         << QStringLiteral("--output") << QStringLiteral("json");
    if (!region.isEmpty()) {
        args << QStringLiteral("--region") << region;
    }
    args << QStringLiteral("--volume-ids");
    for (const QString &id : volumeIds) {
        args << id;
    }
    args << QStringLiteral("--query")
         << QStringLiteral("Volumes[].{id:VolumeId,size:Size,"
                           "type:VolumeType,iops:Iops}");
    return args;
}

QString cloudFleetScope(const CloudIdentity &identity) {
    switch (identity.provider) {
    case CloudProvider::Aws:
        return identity.region.isEmpty()
                   ? QStringLiteral("this region (the CLI default)")
                   : identity.region;
    case CloudProvider::Azure:
        return identity.accountId.isEmpty()
                   ? QStringLiteral("the CLI's default subscription")
                   : identity.accountId;
    case CloudProvider::Gcp:
        return identity.accountId.isEmpty()
                   ? QStringLiteral("the CLI's default project")
                   : identity.accountId;
    case CloudProvider::Oci:
        return identity.accountId.isEmpty()
                   ? QStringLiteral("the CLI's default compartment")
                   : identity.accountId;
    case CloudProvider::None:
        break;
    }
    return QString();
}

QString cloudFleetCommandText(const CloudIdentity &identity) {
    const QString cli = cloudFleetCli(identity.provider);
    if (cli.isEmpty()) {
        return QString();
    }
    return cli + QLatin1Char(' ') +
           cloudFleetArguments(identity).join(QLatin1Char(' '));
}

QString cloudFleetPlanText(const CloudIdentity &identity,
                           const CloudFleetOptions &options) {
    if (!options.enabled) {
        return fleetDisabledText();
    }
    if (identity.provider == CloudProvider::None) {
        return fleetNoProviderText();
    }
    const QString cli = cloudFleetCli(identity.provider);
    if (identity.provider != CloudProvider::Aws) {
        return QStringLiteral(
                   "check now runs `%1` - one read-only call, %2 ms budget; "
                   "nothing is started, stopped or changed")
            .arg(cloudFleetCommandText(identity))
            .arg(options.timeoutMs);
    }
    QStringList steps;
    if (options.identityAuto) {
        steps << QStringLiteral("`aws sts get-caller-identity`")
              << QStringLiteral("`aws configure get region`");
    }
    steps << QStringLiteral("`aws ec2 describe-regions`")
          << QStringLiteral("one `aws ec2 describe-instances` per region (%1 at "
                            "a time)")
                 .arg(qBound(1, options.regionConcurrency,
                             kCloudFleetRegionConcurrencyMax));
    return QStringLiteral(
               "check now: %1 - read-only, no Cost Explorer, nothing started or "
               "stopped")
        .arg(steps.join(QStringLiteral(" + ")));
}

QString cloudWindowsCmdLine(const QString &program,
                            const QStringList &arguments) {
    QString text = QStringLiteral("/s /c \"\"%1\"").arg(program);
    for (const QString &argument : arguments) {
        text += QLatin1Char(' ');
        text += cloudQuoteArgument(argument);
    }
    text += QLatin1Char('"');
    return text;
}

// --- the step machine ------------------------------------------------------
//
// One session is shared by the polled runner (the HUD) and the blocking probe
// (--dump, tests) so the two cannot drift. A step either starts a short-lived
// process or transforms the previous step's output; only the instances step
// runs more than one process, and only that one is concurrent.

class CloudFleetSession {
public:
    CloudFleetSession() = default;
    ~CloudFleetSession() { abort(); }

    void reset() {
        abort();
        identity = CloudIdentity{};
        status = CloudFleetStatus{};
        cliPath.clear();
        homeRegion.clear();
        pendingRegions.clear();
        queriedRegions.clear();
        regionFailures.clear();
        regionError.clear();
        regionListError.clear();
        rows.clear();
        dropped = 0;
        identityResolved = false;
        regionsRequested = false;
        singleRequested = false;
        detailsRequested = false;
        knownCheck = false;
        baseRows.clear();
        okRegions.clear();
        lastKnownCheckMs = 0;
        imageNames.clear();
        typeSpecs.clear();
        volumeSpecs.clear();
        imageIdsByRegion.clear();
        volumeIdsByRegion.clear();
        pendingImageRegions.clear();
        pendingVolumeRegions.clear();
        pendingTypes.clear();
        startedAtMs = 0;
        wallStartedAtMs = 0;
        lastAttemptMs = 0;
    }

    void abort() {
        for (FleetJob &job : jobs) {
            if (job.process == nullptr) {
                continue;
            }
            if (job.process->state() != QProcess::NotRunning) {
                job.process->terminate();
                if (!job.process->waitForFinished(kCloudFleetKillGraceMs)) {
                    job.process->kill();
                    job.process->waitForFinished(kCloudFleetKillGraceMs);
                }
            }
            delete job.process;
            job.process = nullptr;
        }
        jobs.clear();
        phase = FleetPhase::Idle;
    }

    bool active() const {
        return phase != FleetPhase::Idle && phase != FleetPhase::Done;
    }

    void begin(const CloudIdentity &id, const CloudFleetOptions &opts,
               qint64 nowMs) {
        abort();
        // Keep the last answer on screen while the new one is being read: a
        // 20 s job that blanks its own result looks like a collapse/reopen
        // losing the data (reported live).
        const CloudFleetStatus previous = status;
        identity = id;
        options = opts;
        rows.clear();
        dropped = 0;
        queriedRegions.clear();
        regionFailures.clear();
        regionError.clear();
        regionListError.clear();
        pendingRegions.clear();
        homeRegion = id.region;
        identityResolved = false;
        regionsRequested = false;
        singleRequested = false;
        detailsRequested = false;
        knownCheck = false;
        baseRows.clear();
        okRegions.clear();
        imageIdsByRegion.clear();
        volumeIdsByRegion.clear();
        pendingImageRegions.clear();
        pendingVolumeRegions.clear();
        pendingTypes.clear();
        startedAtMs = nowMs;
        wallStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        lastAttemptMs = nowMs;

        status = CloudFleetStatus{};
        if (previous.provider == id.provider && previous.attempted) {
            status.rows = previous.rows;
            status.dropped = previous.dropped;
            status.running = previous.running;
            status.unpriced = previous.unpriced;
            status.monthlyUsd = previous.monthlyUsd;
            status.selfIndex = previous.selfIndex;
            status.account = previous.account;
            status.homeRegion = previous.homeRegion;
            status.regions = previous.regions;
            status.regionFailures = previous.regionFailures;
            status.regionError = previous.regionError;
            status.regionListRefused = previous.regionListRefused;
            status.regionListError = previous.regionListError;
            status.identityResolved = previous.identityResolved;
            status.identitySource = previous.identitySource;
            status.fetchedAtMs = previous.fetchedAtMs;
            status.available = previous.available;
            status.wholeAccount = previous.wholeAccount;
        }
        status.attempted = true;
        status.provider = id.provider;
        status.cliName = cloudFleetCli(id.provider);
        status.scope = cloudFleetScope(id);
        status.source = cloudFleetCommandText(id);
        status.account = id.accountId.isEmpty() ? status.account : id.accountId;
        status.homeRegion = homeRegion.isEmpty() ? status.homeRegion : homeRegion;

        cliPath = cloudCliResolve(status.cliName, options.cliPath);
        status.cliPath = cliPath;
        if (cliPath.isEmpty()) {
            fail(fleetCliMissingText(status.cliName), nowMs);
            return;
        }
        if (id.provider == CloudProvider::Aws) {
            phase = FleetPhase::Identity;
            status.note = QStringLiteral(
                "fleet: naming the account (aws sts get-caller-identity) ...");
        } else {
            phase = FleetPhase::Instances;
            status.note = QStringLiteral("fleet: listing %1 (%2) ...")
                              .arg(status.cliName, status.scope);
        }
        advance(nowMs);
    }

    void poll(qint64 nowMs) {
        if (phase == FleetPhase::Idle || phase == FleetPhase::Done) {
            return;
        }
        const qint64 wallNow = QDateTime::currentMSecsSinceEpoch();
        if (wallStartedAtMs > 0 &&
            wallNow - wallStartedAtMs > qint64(options.accountTimeoutMs)) {
            const int queued = pendingRegions.size() + jobs.size();
            abort();
            finishRun(nowMs,
                      QStringLiteral(
                          "fleet: the whole-account listing did not finish "
                          "within %1 s%2 - the next probe tries again")
                          .arg(qMax(10, options.accountTimeoutMs / 1000))
                          .arg(queued > 0
                                   ? QStringLiteral(" (%1 region(s) still "
                                                    "queued)")
                                         .arg(queued)
                                   : QString()));
            return;
        }
        bool finishedAny = false;
        for (int i = jobs.size() - 1; i >= 0; --i) {
            FleetJob &job = jobs[i];
            const bool expired = job.wallStartedAtMs > 0 &&
                                 wallNow - job.wallStartedAtMs >
                                     qint64(options.timeoutMs);
            if (expired && job.process->state() != QProcess::NotRunning) {
                job.process->terminate();
                if (!job.process->waitForFinished(kCloudFleetKillGraceMs)) {
                    job.process->kill();
                    job.process->waitForFinished(kCloudFleetKillGraceMs);
                }
            }
            if (job.process->state() != QProcess::NotRunning) {
                continue;
            }
            const QByteArray out = job.process->readAllStandardOutput();
            const QByteArray err = job.process->readAllStandardError();
            const int exitCode = job.process->exitCode();
            const bool normalExit =
                job.process->exitStatus() == QProcess::NormalExit;
            FleetJob done = job;
            jobs.removeAt(i);
            delete done.process;
            done.process = nullptr;
            handleResult(done, out, err, exitCode, normalExit, expired, nowMs);
            finishedAny = true;
        }
        if (finishedAny) {
            advance(nowMs);
        }
    }

    void advance(qint64 nowMs) {
        for (int guard = 0; guard < 64; ++guard) {
            if (!jobs.isEmpty()) {
                return;
            }
            switch (phase) {
            case FleetPhase::Idle:
            case FleetPhase::Done:
                return;
            case FleetPhase::Identity: {
                if (!options.identityAuto || !identity.accountId.isEmpty()) {
                    phase = (!options.identityAuto || !homeRegion.isEmpty())
                                ? FleetPhase::Regions
                                : FleetPhase::HomeRegion;
                    continue;
                }
                status.note = QStringLiteral(
                    "fleet: naming the account (aws sts get-caller-identity) "
                    "...");
                if (startJob(QStringLiteral("sts"), QString(),
                             cloudAwsIdentityArguments(), nowMs)) {
                    return;
                }
                continue;
            }
            case FleetPhase::HomeRegion: {
                if (!options.identityAuto || !homeRegion.isEmpty()) {
                    phase = FleetPhase::Regions;
                    continue;
                }
                status.note = QStringLiteral(
                    "fleet: reading the aws home region (aws configure get "
                    "region) ...");
                if (startJob(QStringLiteral("region"), QString(),
                             cloudAwsRegionArguments(), nowMs)) {
                    return;
                }
                continue;
            }
            case FleetPhase::Regions: {
                if (homeRegion.isEmpty()) {
                    homeRegion = cloudAwsDefaultRegion();
                    identity.region = homeRegion;
                    status.homeRegion = homeRegion;
                }
                if (!regionsRequested) {
                    status.note = QStringLiteral(
                        "fleet: reading the enabled regions (aws ec2 "
                        "describe-regions) ...");
                    regionsRequested = true;
                    if (startJob(QStringLiteral("regions"), QString(),
                                 cloudAwsDescribeRegionsArguments(homeRegion),
                                 nowMs)) {
                        return;
                    }
                    continue;
                }
                phase = FleetPhase::Instances;
                if (pendingRegions.isEmpty() && queriedRegions.isEmpty()) {
                    pendingRegions = QStringList{homeRegion};
                }
                continue;
            }
            case FleetPhase::Instances: {
                if (identity.provider != CloudProvider::Aws) {
                    if (singleRequested) {
                        finishRun(nowMs, QString());
                        return;
                    }
                    singleRequested = true;
                    if (startJob(QStringLiteral("instances"), QString(),
                                 cloudFleetArguments(identity), nowMs)) {
                        return;
                    }
                    continue;
                }
                const int limit =
                    qBound(1, options.regionConcurrency,
                           kCloudFleetRegionConcurrencyMax);
                if (jobs.isEmpty()) {
                    // A listing is ~1-2 s per region, so the card has to say
                    // what it is doing rather than only where it started.
                    status.note =
                        QStringLiteral("fleet: listing %1 region(s), %2 at a "
                                       "time ...")
                            .arg(pendingRegions.size() + jobs.size())
                            .arg(limit);
                }
                while (jobs.size() < limit && !pendingRegions.isEmpty()) {
                    const QString region = pendingRegions.takeFirst();
                    queriedRegions.append(region);
                    startJob(QStringLiteral("instances"), region,
                             cloudAwsDescribeInstancesArguments(region), nowMs);
                }
                if (jobs.isEmpty() && pendingRegions.isEmpty()) {
                    if (knownCheck) {
                        finishKnownCheck(nowMs);
                        return;
                    }
                    phase = FleetPhase::Details;
                    continue;
                }
                return;
            }
            case FleetPhase::Details: {
                if (!detailsRequested) {
                    detailsRequested = true;
                    buildDetailPlan();
                }
                startDetailJobs(nowMs);
                if (jobs.isEmpty()) {
                    finishRun(nowMs, QString());
                    return;
                }
                return;
            }
            }
        }
    }

    bool startJob(const QString &stage, const QString &region,
                  const QStringList &args, qint64 nowMs) {        if (args.isEmpty()) {
            stageFailure(stage, region,
                         QStringLiteral("no command to run"), nowMs);
            return false;
        }
        auto *process = new QProcess();
        cloudStartProcess(process, status.cliName, cliPath, args);
        if (!process->waitForStarted(kCloudFleetStartMs)) {
            const QString error = process->errorString();
            delete process;
            stageFailure(stage, region,
                         QStringLiteral("could not start %1 (%2)")
                             .arg(status.cliName, error),
                         QDateTime::currentMSecsSinceEpoch());
            return false;
        }
        FleetJob job;
        job.process = process;
        job.stage = stage;
        job.region = region;
        job.wallStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        jobs.append(job);
        return true;
    }

    void handleResult(const FleetJob &job, const QByteArray &out,
                      const QByteArray &err, int exitCode, bool normalExit,
                      bool timedOut, qint64 nowMs) {
        if (timedOut) {
            stageFailure(job.stage, job.region,
                         fleetTimeoutText(status.cliName,
                                          qMax(1, options.timeoutMs / 1000)),
                         nowMs);
            return;
        }
        if (!normalExit || exitCode != 0) {
            const QString errors = QString::fromLocal8Bit(err);
            stageFailure(job.stage, job.region,
                         cloudFleetNeedsCredentials(identity.provider, errors)
                             ? fleetCredentialsText(identity.provider)
                             : fleetFailureText(status.cliName, exitCode,
                                                cloudFirstLine(errors)),
                         nowMs);
            return;
        }
        if (job.stage == QLatin1String("sts")) {
            QString account;
            QString arn;
            if (!parseAwsCallerIdentity(out, &account, &arn)) {
                stageFailure(job.stage, job.region,
                             QStringLiteral("fleet: aws sts get-caller-identity "
                                            "answered an unexpected document"),
                             nowMs);
                return;
            }
            identity.accountId = account;
            status.account = account;
            identityResolved = true;
            status.identitySource =
                QStringLiteral("aws sts get-caller-identity");
            return;
        }
        if (job.stage == QLatin1String("region")) {
            const QString region = cloudFirstLine(QString::fromLocal8Bit(out));
            homeRegion = region.isEmpty() ? cloudAwsDefaultRegion() : region;
            identity.region = homeRegion;
            status.homeRegion = homeRegion;
            return;
        }
        if (job.stage == QLatin1String("regions")) {
            QStringList regions;
            if (!parseAwsRegionList(out, &regions)) {
                noteRegionsFailure(
                    QStringLiteral("the region list was not readable"), nowMs);
                return;
            }
            pendingRegions = regions;
            // The region this machine lives in is queried first: its row is
            // usually the one being looked for, and a whole account takes ~20 s.
            if (!homeRegion.isEmpty() &&
                pendingRegions.removeAll(homeRegion) > 0) {
                pendingRegions.prepend(homeRegion);
            }
            return;
        }
        if (job.stage == QLatin1String("images")) {
            const QHash<QString, QString> names = parseAwsImageNames(out);
            for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
                imageNames.insert(it.key(), it.value());
            }
            applyDetails();
            return;
        }
        if (job.stage == QLatin1String("volumes")) {
            const QHash<QString, QString> specs = parseAwsVolumeSpecs(out);
            for (auto it = specs.constBegin(); it != specs.constEnd(); ++it) {
                volumeSpecs.insert(it.key(), it.value());
            }
            applyDetails();
            return;
        }
        if (job.stage == QLatin1String("types")) {
            const QHash<QString, QString> specs =
                parseAwsInstanceTypeSpecs(out);
            for (auto it = specs.constBegin(); it != specs.constEnd(); ++it) {
                typeSpecs.insert(it.key(), it.value());
            }
            applyDetails();
            return;
        }
        // instances
        if (out.trimmed().isEmpty()) {
            stageFailure(job.stage, job.region, QStringLiteral("no output"),
                         nowMs);
            return;
        }
        QJsonParseError parseError{};
        QJsonDocument::fromJson(out, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            stageFailure(job.stage, job.region,
                         QStringLiteral("output is not JSON (%1)")
                             .arg(parseError.errorString()),
                         nowMs);
            return;
        }
        const CloudFleetStatus parsed =
            parseCloudFleetJson(identity.provider, out);
        for (CloudFleetInstance row : parsed.rows) {
            // The region this call asked about is authoritative: a projected
            // document carries no region at all, and a per-region listing must
            // be able to say which region each row came from.
            if (!job.region.isEmpty()) {
                row.region = job.region;
            }
            if (rows.size() >= kCloudFleetRowCap) {
                ++dropped;
                continue;
            }
            rows.append(row);
        }
        okRegions.insert(job.region);
        applyDetails();
        // Publish the rows that have arrived instead of waiting for all 18
        // regions: the answer for the region this machine lives in is already
        // known, and the page should show it rather than a spinner for 20 s.
        publishPartial(nowMs);
    }

    void beginKnownCheck(const CloudIdentity &id, const CloudFleetOptions &opts,
                         qint64 nowMs) {
        abort();
        identity = id;
        options = opts;
        knownCheck = true;
        detailsRequested = true;
        baseRows = rows;
        rows.clear();
        dropped = 0;
        queriedRegions.clear();
        okRegions.clear();
        pendingRegions.clear();
        regionFailures.clear();
        regionError.clear();
        identityResolved = status.identityResolved;
        startedAtMs = nowMs;
        wallStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        lastAttemptMs = status.fetchedAtMs > 0 ? status.fetchedAtMs : nowMs;
        // Only the regions that already have a row: one call for the common
        // case, and it catches a stop/start within the minute.
        QStringList regions;
        for (const CloudFleetInstance &row : baseRows) {
            if (!row.region.isEmpty() && !regions.contains(row.region)) {
                regions.append(row.region);
            }
        }
        pendingRegions = regions;
        status.note = QStringLiteral("fleet: re-checking %1 region(s) ...")
                          .arg(regions.size());
        phase = FleetPhase::Instances;
        advance(nowMs);
    }

    void finishKnownCheck(qint64 nowMs) {
        abort();
        // The regions that answered are replaced wholesale; a region whose call
        // failed keeps its last known rows rather than vanishing.
        QList<CloudFleetInstance> merged;
        for (const CloudFleetInstance &row : baseRows) {
            if (okRegions.contains(row.region)) {
                continue;
            }
            merged.append(row);
        }
        for (const CloudFleetInstance &row : rows) {
            merged.append(row);
        }
        status.rows = merged;
        fleetFinalize(status);
        status.fetchedAtMs = nowMs;
        status.note = regionFailures.isEmpty()
                          ? QStringLiteral("fleet: re-checked %1 region(s) just "
                                           "now")
                                .arg(queriedRegions.size())
                          : QStringLiteral("fleet: re-checked %1 region(s) - %2 "
                                           "did not answer")
                                .arg(queriedRegions.size() -
                                     regionFailures.size())
                                .arg(regionFailures.join(
                                    QStringLiteral(", ")));
        lastKnownCheckMs = nowMs;
        phase = FleetPhase::Done;
    }

    void buildDetailPlan() {
        imageIdsByRegion.clear();
        volumeIdsByRegion.clear();
        pendingImageRegions.clear();
        pendingVolumeRegions.clear();
        pendingTypes.clear();
        if (identity.provider != CloudProvider::Aws) {
            return;
        }
        for (const CloudFleetInstance &row : rows) {
            if (!row.imageId.isEmpty() && !imageNames.contains(row.imageId)) {
                QStringList &ids = imageIdsByRegion[row.region];
                if (!ids.contains(row.imageId)) {
                    ids.append(row.imageId);
                }
            }
            if (!row.rootVolumeId.isEmpty() &&
                !volumeSpecs.contains(row.rootVolumeId)) {
                QStringList &ids = volumeIdsByRegion[row.region];
                if (!ids.contains(row.rootVolumeId)) {
                    ids.append(row.rootVolumeId);
                }
            }
        }
        pendingVolumeRegions = volumeIdsByRegion.keys();
        std::sort(pendingVolumeRegions.begin(), pendingVolumeRegions.end());
        pendingImageRegions = imageIdsByRegion.keys();
        std::sort(pendingImageRegions.begin(), pendingImageRegions.end());
        for (const CloudFleetInstance &row : rows) {
            if (!row.instanceType.isEmpty() &&
                !typeSpecs.contains(row.instanceType) &&
                !pendingTypes.contains(row.instanceType)) {
                pendingTypes.append(row.instanceType);
            }
        }
    }

    void startDetailJobs(qint64 nowMs) {
        if (!pendingTypes.isEmpty()) {
            const QString region =
                homeRegion.isEmpty()
                    ? (rows.isEmpty() ? QString() : rows.first().region)
                    : homeRegion;
            const QStringList types = pendingTypes;
            pendingTypes.clear();
            startJob(QStringLiteral("types"), region,
                     cloudAwsDescribeInstanceTypesArguments(region, types),
                     nowMs);
        }
        const int limit = qBound(1, options.regionConcurrency,
                                 kCloudFleetRegionConcurrencyMax);
        while (jobs.size() < limit && !pendingImageRegions.isEmpty()) {
            const QString region = pendingImageRegions.takeFirst();
            const QStringList ids = imageIdsByRegion.value(region);
            if (ids.isEmpty()) {
                continue;
            }
            startJob(QStringLiteral("images"), region,
                     cloudAwsDescribeImagesArguments(region, ids), nowMs);
        }
        while (jobs.size() < limit && !pendingVolumeRegions.isEmpty()) {
            const QString region = pendingVolumeRegions.takeFirst();
            const QStringList ids = volumeIdsByRegion.value(region);
            if (ids.isEmpty()) {
                continue;
            }
            startJob(QStringLiteral("volumes"), region,
                     cloudAwsDescribeVolumesArguments(region, ids), nowMs);
        }
    }

    void applyDetails() {
        for (CloudFleetInstance &row : rows) {
            if (row.imageName.isEmpty()) {
                const auto it = imageNames.constFind(row.imageId);
                if (it != imageNames.constEnd()) {
                    row.imageName = it.value();
                }
            }
            if (row.typeSpec.isEmpty()) {
                const auto it = typeSpecs.constFind(row.instanceType);
                if (it != typeSpecs.constEnd()) {
                    row.typeSpec = it.value();
                }
            }
            if (row.rootVolume.isEmpty()) {
                const auto it = volumeSpecs.constFind(row.rootVolumeId);
                if (it != volumeSpecs.constEnd()) {
                    row.rootVolume = it.value();
                }
            }
        }
    }

    void publishPartial(qint64 nowMs) {
        Q_UNUSED(nowMs);
        if (phase != FleetPhase::Instances || knownCheck) {
            return;
        }
        // Replace the rows only when something has actually arrived: a refresh
        // keeps the previous answer on screen until the new one has a row to
        // show (a region with no instances must not blank it).
        if (!rows.isEmpty()) {
            status.rows = rows;
            status.dropped = dropped;
            fleetFinalize(status);
        }
        status.regions = queriedRegions;
        status.regionFailures = regionFailures;
        status.regionError = regionError;
        status.homeRegion = homeRegion;
        status.account = identity.accountId;
        status.identityResolved = identityResolved;
        status.note = QStringLiteral("fleet: listing %1 region(s) - %2 found "
                                     "so far ...")
                          .arg(queriedRegions.size() + pendingRegions.size())
                          .arg(rows.size());
        status.scope = QStringLiteral("all enabled regions (%1)")
                           .arg(queriedRegions.size() + pendingRegions.size());
    }

    void stageFailure(const QString &stage, const QString &region,
                      const QString &note, qint64 nowMs) {
        if (stage == QLatin1String("sts")) {
            // sts:GetCallerIdentity needs no permission, so a failure here is
            // a broken login (or a broken config), not a restricted key.
            fail(note, nowMs);
            return;
        }
        if (stage == QLatin1String("region")) {
            // No region in ~/.aws/config is normal; the caller falls back.
            homeRegion = cloudAwsDefaultRegion();
            identity.region = homeRegion;
            status.homeRegion = homeRegion;
            return;
        }
        if (stage == QLatin1String("regions")) {
            noteRegionsFailure(note, nowMs);
            return;
        }
        if (stage == QLatin1String("images") || stage == QLatin1String("types") ||
            stage == QLatin1String("volumes")) {
            // Enrichment only: the listing itself succeeded, so a refused
            // describe-images must not make the account look broken.
            return;
        }
        const QString label =
            region.isEmpty() ? QStringLiteral("listing") : region;
        if (!regionFailures.contains(label)) {
            regionFailures.append(label);
        }
        if (regionError.isEmpty()) {
            regionError = note.left(160);
        }
    }

    void noteRegionsFailure(const QString &reason, qint64 nowMs) {
        // IAM can allow DescribeInstances and deny DescribeRegions. Listing
        // the home region is still useful, and the card says exactly why it is
        // the only one.
        status.regionListRefused = true;
        regionListError = reason.left(160);
        pendingRegions = QStringList{homeRegion.isEmpty()
                                         ? cloudAwsDefaultRegion()
                                         : homeRegion};
        phase = FleetPhase::Instances;
        Q_UNUSED(nowMs);
    }

    void fail(const QString &note, qint64 nowMs) {
        abort();
        status.note = note;
        status.fetchedAtMs = nowMs;
        status.rows.clear();
        status.available = false;
        status.running = 0;
        status.unpriced = 0;
        status.dropped = 0;
        status.monthlyUsd = -1.0;
        status.selfIndex = -1;
        rows.clear();
        phase = FleetPhase::Done;
    }

    QString sourceText() const {
        if (identity.provider != CloudProvider::Aws) {
            return cloudFleetCommandText(identity);
        }
        if (queriedRegions.size() <= 1) {
            const QString region =
                queriedRegions.isEmpty() ? homeRegion : queriedRegions.first();
            return QStringLiteral(
                       "%1 ec2 describe-instances --output json --region %2")
                .arg(status.cliName, region);
        }
        return QStringLiteral(
                   "%1 ec2 describe-regions + %1 ec2 describe-instances per "
                   "region (%2)")
            .arg(status.cliName)
            .arg(queriedRegions.size());
    }

    void finishRun(qint64 nowMs, const QString &overrideNote) {
        abort();
        applyDetails();
        status.rows = rows;
        status.dropped = dropped;
        status.regions = queriedRegions;
        status.regionFailures = regionFailures;
        status.regionError = regionError;
        status.regionListError = regionListError;
        status.homeRegion = homeRegion;
        status.account = identity.accountId;
        status.identityResolved = identityResolved;
        status.wholeAccount = queriedRegions.size() > 1;
        status.fetchedAtMs = nowMs;
        if (identity.provider == CloudProvider::Aws &&
            queriedRegions.size() > 1) {
            status.scope = QStringLiteral("all enabled regions (%1)")
                               .arg(queriedRegions.size());
        } else if (queriedRegions.size() == 1) {
            status.scope = queriedRegions.first();
        }
        status.source = sourceText();
        fleetFinalize(status);
        fleetResolveAges(status, nowMs);
        lastKnownCheckMs = nowMs;

        status.note = overrideNote;
        if (status.rows.isEmpty()) {
            if (!overrideNote.isEmpty()) {
                status.note = overrideNote;
            } else if (!queriedRegions.isEmpty() &&
                       regionFailures.size() == queriedRegions.size() &&
                       !regionError.isEmpty()) {
                // Every region failed for the same reason (a logged-out CLI, a
                // denied key): that reason is the answer, not "0 instances".
                status.note = regionError;
            } else {
                status.note = fleetEmptyText(status);
            }
        } else if (!regionFailures.isEmpty() || status.regionListRefused) {
            status.note = overrideNote;
        }
        phase = FleetPhase::Done;
    }

    CloudFleetOptions options;
    CloudIdentity identity;
    CloudFleetStatus status;
    QString cliPath;
    QString homeRegion;
    QStringList pendingRegions;
    QStringList queriedRegions;
    QStringList regionFailures;
    QString regionError;
    QString regionListError;
    QList<CloudFleetInstance> rows;
    int dropped = 0;
    bool identityResolved = false;
    bool regionsRequested = false;
    bool singleRequested = false;
    bool detailsRequested = false;
    // A targeted re-check (see knownCadenceMs): only the regions that already
    // have rows are listed again, and their rows are replaced wholesale.
    bool knownCheck = false;
    QList<CloudFleetInstance> baseRows;
    QSet<QString> okRegions;
    qint64 lastKnownCheckMs = 0;
    // AMI id -> description and instance type -> spec, kept across runs so a
    // second listing of the same account costs no enrichment call at all.
    QHash<QString, QString> imageNames;
    QHash<QString, QString> typeSpecs;
    QHash<QString, QString> volumeSpecs;
    QHash<QString, QStringList> imageIdsByRegion;
    QHash<QString, QStringList> volumeIdsByRegion;
    QStringList pendingImageRegions;
    QStringList pendingVolumeRegions;
    QStringList pendingTypes;
    QList<FleetJob> jobs;
    FleetPhase phase = FleetPhase::Idle;
    qint64 startedAtMs = 0;
    qint64 wallStartedAtMs = 0;
    qint64 lastAttemptMs = 0;
};

CloudFleetStatus probeCloudFleet(const CloudIdentity &identity,
                                 const CloudFleetOptions &options,
                                 qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (!options.enabled) {
        CloudFleetStatus status;
        status.note = fleetDisabledText();
        return status;
    }
    if (identity.provider == CloudProvider::None) {
        CloudFleetStatus status;
        status.note = fleetNoProviderText();
        return status;
    }
    CloudFleetSession session;
    session.begin(identity, options, nowMs);
    const qint64 budget = qBound(10000, options.accountTimeoutMs, 300000);
    const qint64 wallStart = QDateTime::currentMSecsSinceEpoch();
    while (session.active()) {
        if (QDateTime::currentMSecsSinceEpoch() - wallStart > budget) {
            session.abort();
            session.finishRun(
                nowMs,
                QStringLiteral("fleet: the whole-account listing did not "
                               "finish within %1 s")
                    .arg(budget / 1000));
            break;
        }
        bool waited = false;
        for (const FleetJob &job : session.jobs) {
            if (job.process != nullptr &&
                job.process->state() != QProcess::NotRunning) {
                job.process->waitForFinished(kCloudFleetPollSliceMs);
                waited = true;
            }
        }
        session.poll(nowMs);
        if (!waited && session.active() && session.jobs.isEmpty()) {
            // A step that starts no process advances in poll/advance; loop.
            continue;
        }
    }
    return session.status;
}

CloudFleetRunner::CloudFleetRunner(QObject *parent)
    : QObject(parent)
    , m_session(new CloudFleetSession()) {
}

CloudFleetRunner::~CloudFleetRunner() = default;

void CloudFleetRunner::setOptions(const CloudFleetOptions &options) {
    m_options = options;
    if (m_options.cadenceMs <= 0) {
        m_options.cadenceMs = kCloudFleetCadenceMs;
    }
}

CloudFleetOptions CloudFleetRunner::options() const {
    return m_options;
}

const CloudFleetStatus &CloudFleetRunner::status() const {
    return m_session->status;
}

void CloudFleetRunner::refreshNow() {
    // A request during a run is queued, not dropped: clicking CHECK NOW while
    // the listing is already in flight used to do nothing at all, which looks
    // exactly like a broken button.
    m_refreshPending = true;
}

void CloudFleetRunner::stop() {
    m_session->abort();
    m_session->lastAttemptMs = 0;
    m_refreshPending = false;
}

void CloudFleetRunner::update(const CloudIdentity &identity, qint64 nowMs) {
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    // One line per finished listing: "did the account inventory actually run,
    // and what did it say" must be answerable from hud.log, because the page
    // only speaks after a run and a silent no-op is indistinguishable from a
    // broken one.
    const auto logFinished = [this]() {
        const CloudFleetStatus &status = m_session->status;
        qInfo("[cloud] inventory: %d row(s), %d region(s), %s - %s",
              int(status.rows.size()), int(status.regions.size()),
              qUtf8Printable(status.cliName.isEmpty()
                                 ? QStringLiteral("no cli")
                                 : status.cliName),
              qUtf8Printable(status.note.isEmpty() ? status.scope
                                                   : status.note));
    };
    if (!m_options.enabled) {
        m_session->reset();
        m_session->status.note = fleetDisabledText();
        return;
    }
    if (identity.provider == CloudProvider::None) {
        m_session->reset();
        m_session->status.note = fleetNoProviderText();
        return;
    }
    if (m_session->active()) {
        m_session->poll(nowMs);
        if (m_session->active()) {
            return;
        }
        logFinished();
    }
    if (m_refreshPending) {
        m_refreshPending = false;
        m_session->reset();
        qInfo("[cloud] inventory starting (on request)");
        m_session->begin(identity, m_options, nowMs);
        if (!m_session->active()) {
            logFinished();
        }
        return;
    }
    // A different provider or a different machine invalidates the listing.
    // NOT the account or the region: those are what the run itself resolves
    // (`sts`, `configure get region`), so the caller's copy is still empty
    // while the session has already filled them in. Comparing them reset the
    // session on the very next tick, so the listing restarted every ~20 s and
    // the card only ever showed "naming the account ..." (found live).
    if (m_session->lastAttemptMs > 0 &&
        (identity.provider != m_session->identity.provider ||
         identity.instanceId != m_session->identity.instanceId)) {
        m_session->reset();
    }
    if (m_session->lastAttemptMs > 0 &&
        nowMs - m_session->lastAttemptMs < m_options.cadenceMs) {
        // Between full sweeps, re-list only the regions that already have
        // rows: one call catches a stopped or started instance in seconds,
        // where the full 18-region sweep would cost 18 processes for the same
        // answer.
        if (m_options.knownCadenceMs > 0 && !m_session->status.rows.isEmpty() &&
            m_session->lastKnownCheckMs > 0 &&
            nowMs - m_session->lastKnownCheckMs >= m_options.knownCadenceMs) {
            m_session->beginKnownCheck(identity, m_options, nowMs);
            if (!m_session->active()) {
                logFinished();
            }
        }
        return;
    }
    qInfo("[cloud] inventory starting (on cadence)");
    m_session->begin(identity, m_options, nowMs);
    if (!m_session->active()) {
        logFinished();
    }
}
