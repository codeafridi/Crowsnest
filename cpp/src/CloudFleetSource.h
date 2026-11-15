#pragma once

#include "CloudStatus.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QProcess;

// Account inventory through the provider's own CLI (M83). The metadata service
// answers "which instance is this box"; the CLI answers "what else is in the
// account, and when did this box launch".
//
// Crowsnest never reads or stores credentials: the CLI runs with the profile the
// user already configured, and a refusal is reported with the command that
// fixes it. The command is spawned once per cadence, never per tick, and the
// sampler polls it instead of waiting.
//
// AWS is a special case because `ec2 describe-instances` is region-scoped: a
// listing of the whole account is `sts get-caller-identity` (which account) +
// `ec2 describe-regions` (which regions are enabled) + one
// `describe-instances` per region, run through a small pool. Starting an
// instance in any region therefore shows up here, which a single-region
// listing cannot do.
//
// Grounding (each provider's shipped client, kept /tmp/opencode/m83 and
// /home/nullchan/aws/dist/awscli/botocore):
//   aws-cli 2.34.53: `aws ec2 describe-instances help` - "Describes the
//     specified instances or all instances... If you do not specify instance
//     IDs or filters, the output includes information for all instances";
//     `--query` "A JMESPath query to use in filtering the response data";
//     `--region` "The region to use. Overrides config/env settings."
//     botocore sts/2011-06-15: GetCallerIdentity output `UserId`/`Account`/
//     `Arn`, and "No permissions are required to perform this operation."
//     botocore ec2/2016-11-15: DescribeRegions "Describes the Regions that are
//     enabled for your account, or all Regions."; the CLI defaults to the
//     enabled set; "The order of the elements in the response ... might vary."
//   azure-cli-vm 2.2.23 _params.py `--show-details, -d` "show public ip
//     address, FQDN, and power states. command will run slow"; custom.py
//     list_vm(cmd, resource_group_name=None, show_details=False);
//     azure-cli-core 2.91.0 arm.py `--subscription` "Name or ID of
//     subscription. You can configure the default subscription using `az
//     account set -s NAME_OR_ID`".
//   google-cloud-cli calliope/base.py FORMAT_FLAG "--format" "Sets the format
//     for printing command output resources."; parser_arguments.py lists
//     "--project" among the global flags (base.py: "set ``--project'' flag to
//     PROJECT ID"); surface/compute/instances/list.py "List Compute Engine
//     virtual machine instances."
//   oci-cli 3.95.1 cli_root.py:438 "--region" "The region to make calls
//     against."; compute_cli.py list_instances(..., compartment_id, ...)
//     with "--all" "Fetches all pages of results" and "--output" "The output
//     format. [Default is json]".
struct CloudFleetOptions {
    bool enabled = true;
    int timeoutMs = 25000;
    // Absolute path to the provider CLI. Empty means "resolve like a login
    // shell"; a test (or an operator with several aws installs) pins it so the
    // binary that runs is the one that was verified, not whichever PATH wins.
    QString cliPath;
    // AWS: resolve the account and region through the CLI's own login when the
    // metadata service found nothing. Set by `[cloud] identity=auto`.
    bool identityAuto = false;
    // describe-instances calls in flight at once: each `aws` spawn is ~1-2 s
    // of Python startup and a whole account is ~30 regions.
    int regionConcurrency = 4;
    // The whole-account listing (identity + regions + instances) has its own
    // budget, separate from the per-call timeout.
    int accountTimeoutMs = 90000;
    // Poll cadence; the sampler shortens it while the Cloud page is visible.
    qint64 cadenceMs = 600000;
};

QString cloudFleetCli(CloudProvider provider);

QStringList cloudFleetArguments(const CloudIdentity &identity);

QString cloudFleetScope(const CloudIdentity &identity);

QString cloudFleetCommandText(const CloudIdentity &identity);

// The three AWS calls the whole-account listing makes, one argument list each.
QStringList cloudAwsIdentityArguments();
QStringList cloudAwsRegionArguments();
QStringList cloudAwsDescribeRegionsArguments(const QString &homeRegion);
QStringList cloudAwsDescribeInstancesArguments(const QString &region);

// What "CHECK NOW" runs, in words: every command, how many times, and the two
// promises (read only; Cost Explorer is not called).
QString cloudFleetPlanText(const CloudIdentity &identity,
                           const CloudFleetOptions &options);

// Windows installs `az` and `gcloud` as .cmd shims, which CreateProcess cannot
// start directly, so the CLI runs through the command processor:
//   cmd.exe /s /c ""<cli>" <args>"
QString cloudWindowsCmdLine(const QString &program,
                            const QStringList &arguments);

// Synchronous one-shot used by `--dump`. The HUD uses CloudFleetRunner so the
// sampler thread never waits on a CLI.
CloudFleetStatus probeCloudFleet(const CloudIdentity &identity,
                                 const CloudFleetOptions &options,
                                 qint64 nowMs = 0);

class CloudFleetSession;

class CloudFleetRunner : public QObject {
    Q_OBJECT

public:
    explicit CloudFleetRunner(QObject *parent = nullptr);
    ~CloudFleetRunner() override;

    void setOptions(const CloudFleetOptions &options);
    CloudFleetOptions options() const;

    const CloudFleetStatus &status() const;

    // Starts the listing when the cadence expires, collects the finished
    // processes and starts the next step, and kills the run past the deadline.
    // Never blocks.
    void update(const CloudIdentity &identity, qint64 nowMs);

    // "CHECK NOW": lets the next update() start immediately. A run already in
    // flight is left alone rather than restarted.
    void refreshNow();

    void stop();

private:
    CloudFleetOptions m_options;
    std::unique_ptr<CloudFleetSession> m_session;
    bool m_refreshPending = false;
};
