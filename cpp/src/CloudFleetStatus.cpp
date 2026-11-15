#include "QtCompat.h"
#include "CloudStatus.h"

#include "CostModel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>

namespace {

QString field(const QJsonObject &object, const char *key) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(qint64(value.toDouble()));
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return QString();
}

// Some providers wrap the interesting value one level down (AWS
// `State.Name`, Azure `properties.hardwareProfile.vmSize`); the projected
// AWS form and an older CLI flatten it to the top level, so accept both.
QString nestedField(const QJsonObject &object, const char *outer,
                    const char *inner) {
    const QJsonValue value = object.value(QLatin1String(outer));
    if (value.isObject()) {
        const QString inner_value = field(value.toObject(), inner);
        if (!inner_value.isEmpty()) {
            return inner_value;
        }
    }
    if (value.isString()) {
        return value.toString();
    }
    return QString();
}

qint64 timestampMs(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }
    QDateTime moment = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!moment.isValid()) {
        moment = QDateTime::fromString(trimmed, Qt::ISODate);
    }
    if (!moment.isValid()) {
        return -1;
    }
    if (moment.timeSpec() == Qt::LocalTime) {
        moment.setTimeZone(qtcompat::utcZone());
    }
    return moment.toMSecsSinceEpoch();
}

QStringList tagsFromArray(const QJsonArray &tags) {
    QStringList out;
    for (const QJsonValue &value : tags) {
        const QJsonObject tag = value.toObject();
        const QString key = field(tag, "Key");
        if (key.isEmpty()) {
            continue;
        }
        out.append(QStringLiteral("%1=%2").arg(key, field(tag, "Value")));
    }
    return out;
}

QStringList tagsFromObject(const QJsonObject &tags) {
    QStringList out;
    const QStringList keys = tags.keys();
    for (const QString &key : keys) {
        out.append(QStringLiteral("%1=%2").arg(key, tags.value(key).toString()));
    }
    return out;
}

QString tagValue(const QStringList &tags, const QString &key) {
    const QString prefix = key + QLatin1Char('=');
    for (const QString &tag : tags) {
        if (tag.startsWith(prefix)) {
            return tag.mid(prefix.size());
        }
    }
    return QString();
}

QString azureGroup(const QJsonObject &object) {
    QString group = field(object, "resourceGroup");
    if (!group.isEmpty()) {
        return group;
    }
    const QString id = field(object, "id");
    const int index =
        id.indexOf(QLatin1String("/resourceGroups/"), 0, Qt::CaseInsensitive);
    if (index < 0) {
        return QString();
    }
    return id.mid(index + 16).section(QLatin1Char('/'), 0, 0);
}

QString azurePowerState(const QJsonObject &object) {
    QString state = field(object, "powerState");
    if (state.isEmpty()) {
        state = field(object, "power_state");
    }
    if (!state.isEmpty()) {
        return state;
    }
    QJsonObject view = object.value(QStringLiteral("instanceView")).toObject();
    if (view.isEmpty()) {
        const QJsonObject properties =
            object.value(QStringLiteral("properties")).toObject();
        view = properties.value(QStringLiteral("instanceView")).toObject();
    }
    const QJsonArray statuses = view.value(QStringLiteral("statuses")).toArray();
    for (const QJsonValue &value : statuses) {
        const QJsonObject status = value.toObject();
        if (!field(status, "code").startsWith(QLatin1String("PowerState/"))) {
            continue;
        }
        const QString display = field(status, "displayStatus");
        return display.isEmpty() ? field(status, "code") : display;
    }
    return QString();
}

void appendRow(CloudFleetStatus &fleet, CloudFleetInstance row) {
    row.rawState = row.rawState.trimmed();
    row.state = fleetState(fleet.provider, row.rawState);
    if (row.region.isEmpty()) {
        row.region = cloudRegionFromZone(row.zone);
    }
    if (row.name.isEmpty()) {
        row.name = tagValue(row.tags, QStringLiteral("Name"));
    }
    if (row.name.isEmpty()) {
        row.name = row.instanceId;
    }
    fleet.rows.append(row);
}

void finishFleet(CloudFleetStatus &fleet) {
    fleet.available = !fleet.rows.isEmpty();
    int running = 0;
    for (const CloudFleetInstance &row : fleet.rows) {
        if (row.state == QLatin1String("running")) {
            ++running;
        }
    }
    fleet.running = running;
    // Self first, then running, then by name: a long account stays readable
    // and the row the page is about never scrolls away.
    std::sort(fleet.rows.begin(), fleet.rows.end(),
              [](const CloudFleetInstance &left, const CloudFleetInstance &right) {
                  const bool leftRunning = left.state == QLatin1String("running");
                  const bool rightRunning = right.state == QLatin1String("running");
                  if (left.state != right.state && leftRunning != rightRunning) {
                      return leftRunning;
                  }
                  return left.name.localeAwareCompare(right.name) < 0;
              });
}

} // namespace

CloudFleetStatus parseAwsDescribeInstances(const QByteArray &body) {
    CloudFleetStatus fleet;
    fleet.provider = CloudProvider::Aws;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    QList<QJsonObject> instances;
    if (document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            if (value.isObject()) {
                instances.append(value.toObject());
            }
        }
    } else if (document.isObject()) {
        const QJsonArray reservations =
            document.object().value(QStringLiteral("Reservations")).toArray();
        for (const QJsonValue &reservation : reservations) {
            const QJsonArray group = reservation.toObject()
                                         .value(QStringLiteral("Instances"))
                                         .toArray();
            for (const QJsonValue &instance : group) {
                if (instance.isObject()) {
                    instances.append(instance.toObject());
                }
            }
        }
    }
    for (const QJsonObject &object : instances) {
        CloudFleetInstance row;
        row.instanceId = field(object, "InstanceId");
        if (row.instanceId.isEmpty()) {
            row.instanceId = field(object, "id");
        }
        row.instanceType = field(object, "InstanceType");
        if (row.instanceType.isEmpty()) {
            row.instanceType = field(object, "type");
        }
        row.rawState = nestedField(object, "State", "Name");
        if (row.rawState.isEmpty()) {
            row.rawState = field(object, "state");
        }
        row.zone = nestedField(object, "Placement", "AvailabilityZone");
        if (row.zone.isEmpty()) {
            row.zone = field(object, "az");
        }
        row.region = field(object, "region");
        row.privateIp = field(object, "PrivateIpAddress");
        if (row.privateIp.isEmpty()) {
            row.privateIp = field(object, "privateIp");
        }
        row.publicIp = field(object, "PublicIpAddress");
        if (row.publicIp.isEmpty()) {
            row.publicIp = field(object, "publicIp");
        }
        row.launchedAtMs = timestampMs(field(object, "LaunchTime"));
        if (row.launchedAtMs < 0) {
            row.launchedAtMs = timestampMs(field(object, "launched"));
        }
        row.tags = tagsFromArray(object.value(QStringLiteral("Tags")).toArray());
        const QString spotId = field(object, "SpotInstanceRequestId");
        row.spot = !spotId.isEmpty() ||
                   object.value(QStringLiteral("spot")).toBool();
        if (row.name.isEmpty()) {
            row.name = field(object, "name");
        }
        row.imageId = field(object, "ImageId");
        row.architecture = field(object, "Architecture");
        row.platformDetails = field(object, "PlatformDetails");
        if (row.platformDetails.isEmpty()) {
            const QString platform = field(object, "Platform");
            if (!platform.isEmpty()) {
                row.platformDetails =
                    platform.compare(QLatin1String("windows"),
                                     Qt::CaseInsensitive) == 0
                        ? QStringLiteral("Windows")
                        : platform;
            }
        }
        row.keyName = field(object, "KeyName");
        row.vpcId = field(object, "VpcId");
        row.subnetId = field(object, "SubnetId");
        row.iamProfile = nestedField(object, "IamInstanceProfile", "Arn");
        if (row.iamProfile.isEmpty()) {
            row.iamProfile = field(object, "iamProfile");
        }
        const QJsonArray securityGroups =
            object.value(QStringLiteral("SecurityGroups")).toArray();
        for (const QJsonValue &value : securityGroups) {
            const QJsonObject group = value.toObject();
            const QString groupName = field(group, "GroupName");
            const QString groupId = field(group, "GroupId");
            if (!groupName.isEmpty() && !groupId.isEmpty()) {
                row.securityGroups.append(
                    QStringLiteral("%1 (%2)").arg(groupName, groupId));
            } else if (!groupName.isEmpty()) {
                row.securityGroups.append(groupName);
            } else if (!groupId.isEmpty()) {
                row.securityGroups.append(groupId);
            }
        }
        row.tenancy = field(object, "Tenancy");
        row.rootDeviceType = field(object, "RootDeviceType");
        row.virtualizationType = field(object, "VirtualizationType");
        row.instanceLifecycle = field(object, "InstanceLifecycle");
        row.httpTokens = nestedField(object, "MetadataOptions", "HttpTokens");
        row.monitoring = nestedField(object, "Monitoring", "State");
        row.creditSpec =
            nestedField(object, "CreditSpecification", "CpuCredits");
        row.stateReason = field(object, "StateTransitionReason");
        row.privateDns = field(object, "PrivateDnsName");
        row.publicDns = field(object, "PublicDnsName");
        const int profileAt = row.iamProfile.lastIndexOf(
            QLatin1Char('/'));
        if (profileAt >= 0) {
            row.iamProfileName = row.iamProfile.mid(profileAt + 1);
        }
        row.rootDevice = field(object, "RootDeviceName");
        const QJsonArray mappings =
            object.value(QStringLiteral("BlockDeviceMappings")).toArray();
        for (const QJsonValue &value : mappings) {
            const QJsonObject mapping = value.toObject();
            const QString device = field(mapping, "DeviceName");
            if (!row.rootDevice.isEmpty() && device != row.rootDevice) {
                continue;
            }
            row.rootVolumeId = nestedField(mapping, "Ebs", "VolumeId");
            if (!row.rootVolumeId.isEmpty()) {
                if (row.rootDevice.isEmpty()) {
                    row.rootDevice = device;
                }
                break;
            }
        }
        if (row.rawState.isEmpty()) {
            row.rawState = QStringLiteral("unknown");
        }
        appendRow(fleet, row);
    }
    finishFleet(fleet);
    return fleet;
}

CloudFleetStatus parseAzureVmList(const QByteArray &body) {
    CloudFleetStatus fleet;
    fleet.provider = CloudProvider::Azure;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isArray()) {
        return fleet;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            continue;
        }
        CloudFleetInstance row;
        row.name = field(object, "name");
        row.group = azureGroup(object);
        row.instanceId = field(object, "vmId");
        if (row.instanceId.isEmpty()) {
            row.instanceId = nestedField(object, "properties", "vmId");
        }
        row.instanceType = nestedField(object, "hardwareProfile", "vmSize");
        if (row.instanceType.isEmpty()) {
            const QJsonObject properties =
                object.value(QStringLiteral("properties")).toObject();
            row.instanceType = nestedField(properties, "hardwareProfile", "vmSize");
        }
        if (row.instanceType.isEmpty()) {
            row.instanceType = field(object, "vmSize");
        }
        row.rawState = azurePowerState(object);
        if (row.rawState.isEmpty()) {
            row.rawState = nestedField(object, "properties", "provisioningState");
        }
        row.region = field(object, "location");
        const QJsonArray zones = object.value(QStringLiteral("zones")).toArray();
        if (!zones.isEmpty()) {
            row.zone = zones.first().toString();
        }
        row.privateIp = field(object, "private_ips");
        if (row.privateIp.isEmpty()) {
            row.privateIp = field(object, "privateIps");
        }
        row.publicIp = field(object, "public_ips");
        if (row.publicIp.isEmpty()) {
            row.publicIp = field(object, "publicIps");
        }
        row.launchedAtMs = timestampMs(field(object, "timeCreated"));
        if (row.launchedAtMs < 0) {
            row.launchedAtMs =
                timestampMs(nestedField(object, "properties", "timeCreated"));
        }
        row.tags = tagsFromObject(object.value(QStringLiteral("tags")).toObject());
        if (row.rawState.isEmpty()) {
            row.rawState = QStringLiteral("unknown");
        }
        appendRow(fleet, row);
    }
    finishFleet(fleet);
    return fleet;
}

CloudFleetStatus parseGcpInstanceList(const QByteArray &body) {
    CloudFleetStatus fleet;
    fleet.provider = CloudProvider::Gcp;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isArray()) {
        return fleet;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            continue;
        }
        CloudFleetInstance row;
        row.instanceId = field(object, "id");
        row.name = field(object, "name");
        row.instanceType = cloudMachineTypeTail(field(object, "machineType"));
        row.rawState = field(object, "status");
        row.zone = cloudMachineTypeTail(field(object, "zone"));
        row.launchedAtMs = timestampMs(field(object, "creationTimestamp"));
        const QJsonArray interfaces =
            object.value(QStringLiteral("networkInterfaces")).toArray();
        if (!interfaces.isEmpty()) {
            const QJsonObject first = interfaces.first().toObject();
            row.privateIp = field(first, "networkIP");
            const QJsonArray access =
                first.value(QStringLiteral("accessConfigs")).toArray();
            if (!access.isEmpty()) {
                row.publicIp = field(access.first().toObject(), "natIP");
            }
        }
        row.tags = tagsFromObject(object.value(QStringLiteral("labels")).toObject());
        const QJsonObject scheduling =
            object.value(QStringLiteral("scheduling")).toObject();
        row.spot = field(scheduling, "provisioningModel")
                       .compare(QLatin1String("SPOT"), Qt::CaseInsensitive) == 0 ||
                   scheduling.value(QStringLiteral("preemptible")).toBool();
        if (row.rawState.isEmpty()) {
            row.rawState = QStringLiteral("unknown");
        }
        appendRow(fleet, row);
    }
    finishFleet(fleet);
    return fleet;
}

CloudFleetStatus parseOciInstanceList(const QByteArray &body) {
    CloudFleetStatus fleet;
    fleet.provider = CloudProvider::Oci;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return fleet;
    }
    const QJsonArray instances =
        document.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : instances) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            continue;
        }
        CloudFleetInstance row;
        row.instanceId = field(object, "id");
        row.name = field(object, "display-name");
        if (row.name.isEmpty()) {
            row.name = field(object, "displayName");
        }
        row.instanceType = field(object, "shape");
        row.rawState = field(object, "lifecycle-state");
        if (row.rawState.isEmpty()) {
            row.rawState = field(object, "lifecycleState");
        }
        row.region = field(object, "region");
        row.zone = field(object, "availability-domain");
        if (row.zone.isEmpty()) {
            row.zone = field(object, "availabilityDomain");
        }
        row.group = field(object, "compartment-id");
        if (row.group.isEmpty()) {
            row.group = field(object, "compartmentId");
        }
        row.launchedAtMs = timestampMs(field(object, "time-created"));
        if (row.launchedAtMs < 0) {
            row.launchedAtMs = timestampMs(field(object, "timeCreated"));
        }
        QJsonObject tags = object.value(QStringLiteral("freeform-tags")).toObject();
        if (tags.isEmpty()) {
            tags = object.value(QStringLiteral("freeformTags")).toObject();
        }
        row.tags = tagsFromObject(tags);
        const QJsonObject preemptible =
            object.value(QStringLiteral("preemptible-instance-config")).toObject();
        row.spot = !preemptible.isEmpty();
        if (row.rawState.isEmpty()) {
            row.rawState = QStringLiteral("unknown");
        }
        appendRow(fleet, row);
    }
    finishFleet(fleet);
    return fleet;
}

CloudFleetStatus parseCloudFleetJson(CloudProvider provider,
                                     const QByteArray &body) {
    switch (provider) {
    case CloudProvider::Aws:
        return parseAwsDescribeInstances(body);
    case CloudProvider::Azure:
        return parseAzureVmList(body);
    case CloudProvider::Gcp:
        return parseGcpInstanceList(body);
    case CloudProvider::Oci:
        return parseOciInstanceList(body);
    case CloudProvider::None:
        break;
    }
    return CloudFleetStatus{};
}

bool parseAwsCallerIdentity(const QByteArray &body, QString *account,
                            QString *arn) {
    if (account != nullptr) {
        account->clear();
    }
    if (arn != nullptr) {
        arn->clear();
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    const QString parsedAccount = field(object, "Account");
    if (parsedAccount.isEmpty()) {
        return false;
    }
    if (account != nullptr) {
        *account = parsedAccount;
    }
    if (arn != nullptr) {
        *arn = field(object, "Arn");
    }
    return true;
}

bool parseAwsRegionList(const QByteArray &body, QStringList *regions) {
    if (regions != nullptr) {
        regions->clear();
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return false;
    }
    const QJsonArray array =
        document.object().value(QStringLiteral("Regions")).toArray();
    QStringList names;
    for (const QJsonValue &value : array) {
        const QString name = field(value.toObject(), "RegionName");
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
        }
    }
    // "The order of the elements ... might vary", so the list is sorted to
    // keep the scope line, the failure list and the tests deterministic.
    std::sort(names.begin(), names.end());
    if (names.isEmpty()) {
        return false;
    }
    if (regions != nullptr) {
        *regions = names;
    }
    return true;
}

QString cloudAwsDefaultRegion() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString region = env.value(QStringLiteral("AWS_REGION")).trimmed();
    if (!region.isEmpty()) {
        return region;
    }
    const QString fallback =
        env.value(QStringLiteral("AWS_DEFAULT_REGION")).trimmed();
    if (!fallback.isEmpty()) {
        return fallback;
    }
    return QStringLiteral("us-east-1");
}

QString fleetState(CloudProvider provider, const QString &rawState) {
    const QString raw = rawState.trimmed().toLower();
    if (raw.isEmpty()) {
        return QStringLiteral("unknown");
    }
    const auto has = [&raw](const char *token) {
        return raw.contains(QLatin1String(token));
    };
    if (has("deallocat")) {
        return QStringLiteral("stopped");
    }
    if (has("shutting-down") || has("shutting down")) {
        return QStringLiteral("terminated");
    }
    if (has("terminat")) {
        // GCP TERMINATED and OCI TERMINATED mean "stopped" (a deleted
        // instance leaves the list); only AWS keeps a terminated instance
        // around, so the word means what it says there.
        return provider == CloudProvider::Aws ? QStringLiteral("terminated")
                                              : QStringLiteral("stopped");
    }
    if (has("suspend") || has("hibernat")) {
        return QStringLiteral("suspended");
    }
    if (has("running") || has("started")) {
        return QStringLiteral("running");
    }
    if (has("stopp") || has("stopped")) {
        return QStringLiteral("stopped");
    }
    if (has("start") || has("pending") || has("provision") || has("staging") ||
        has("repair") || has("moving") || has("creating") || has("reboot") ||
        has("updat")) {
        return QStringLiteral("pending");
    }
    return QStringLiteral("unknown");
}

qint64 fleetAgeSeconds(qint64 launchedAtMs, qint64 nowMs) {
    if (launchedAtMs <= 0 || nowMs <= 0 || launchedAtMs > nowMs) {
        return -1;
    }
    return (nowMs - launchedAtMs) / 1000;
}

void fleetResolveAges(CloudFleetStatus &fleet, qint64 nowMs) {
    for (CloudFleetInstance &row : fleet.rows) {
        row.ageSec = fleetAgeSeconds(row.launchedAtMs, nowMs);
    }
}

void fleetApplyRates(CloudFleetStatus &fleet, const CloudRateTable &table,
                     double hoursPerMonth) {
    int unpriced = 0;
    int running = 0;
    double monthly = 0.0;
    bool priced = false;
    for (CloudFleetInstance &row : fleet.rows) {
        row.ratePerHour = cloudRateFor(table, fleet.provider, row.instanceType);
        // A stopped instance is not billed for compute, so it never carries a
        // monthly projection of its own: the figure would read as money being
        // spent right now.
        row.monthlyUsd = row.ratePerHour < 0.0 ||
                                 row.state != QLatin1String("running")
                             ? -1.0
                             : row.ratePerHour * hoursPerMonth;
        if (row.state != QLatin1String("running")) {
            continue;
        }
        ++running;
        if (row.ratePerHour < 0.0) {
            ++unpriced;
            continue;
        }
        priced = true;
        monthly += row.monthlyUsd;
    }
    fleet.running = running;
    fleet.unpriced = unpriced;
    fleet.monthlyUsd = priced ? monthly : -1.0;
}

int fleetMarkSelf(CloudFleetStatus &fleet, const CloudIdentity &identity) {
    if (identity.provider != fleet.provider || identity.provider == CloudProvider::None) {
        fleet.selfIndex = -1;
        return -1;
    }
    int index = -1;
    if (!identity.instanceId.isEmpty()) {
        for (int i = 0; i < fleet.rows.size(); ++i) {
            if (!fleet.rows[i].instanceId.isEmpty() &&
                fleet.rows[i].instanceId.compare(identity.instanceId,
                                                 Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
    }
    if (index < 0 && !identity.instanceName.isEmpty()) {
        for (int i = 0; i < fleet.rows.size(); ++i) {
            if (fleet.rows[i].name.compare(identity.instanceName,
                                           Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
    }
    // A CLI-resolved identity (no metadata service) knows neither the instance
    // id nor its name, but this host does know its own address, and the
    // listing carries each instance's private address. That is the only
    // reliable key left, so it is used rather than guessing from a hostname.
    if (index < 0 && !identity.privateIp.isEmpty()) {
        for (int i = 0; i < fleet.rows.size(); ++i) {
            if (!fleet.rows[i].privateIp.isEmpty() &&
                fleet.rows[i].privateIp.compare(identity.privateIp,
                                                Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) {
        fleet.selfIndex = -1;
        return -1;
    }
    // The listing is sorted by state/name; the row this page is about always
    // goes first so a long account never scrolls it away.
    if (index > 0) {
        fleet.rows.move(index, 0);
        index = 0;
    }
    for (int i = 0; i < fleet.rows.size(); ++i) {
        fleet.rows[i].isSelf = i == index;
    }
    fleet.selfIndex = index;
    return index;
}

void fleetFinalize(CloudFleetStatus &fleet) {
    finishFleet(fleet);
}

QHash<QString, QString> parseAwsImageNames(const QByteArray &body) {
    QHash<QString, QString> names;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    QJsonArray images;
    if (document.isArray()) {
        images = document.array();
    } else if (document.isObject()) {
        images = document.object().value(QStringLiteral("Images")).toArray();
    }
    for (const QJsonValue &value : images) {
        const QJsonObject object = value.toObject();
        QString id = field(object, "ImageId");
        if (id.isEmpty()) {
            id = field(object, "id");
        }
        if (id.isEmpty()) {
            continue;
        }
        QString label = field(object, "Description");
        if (label.isEmpty()) {
            label = field(object, "desc");
        }
        if (label.isEmpty()) {
            label = field(object, "Name");
        }
        if (label.isEmpty()) {
            label = field(object, "name");
        }
        if (label.isEmpty()) {
            continue;
        }
        // The provider's description is the only place the distribution is
        // named ("Amazon Linux 2023 AMI ...", "Ubuntu 22.04 LTS ...").
        names.insert(id, label.simplified());
    }
    return names;
}

QHash<QString, QString> parseAwsInstanceTypeSpecs(const QByteArray &body) {
    QHash<QString, QString> specs;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    QJsonArray types;
    if (document.isArray()) {
        types = document.array();
    } else if (document.isObject()) {
        types =
            document.object().value(QStringLiteral("InstanceTypes")).toArray();
    }
    for (const QJsonValue &value : types) {
        const QJsonObject object = value.toObject();
        QString type = field(object, "InstanceType");
        if (type.isEmpty()) {
            type = field(object, "type");
        }
        if (type.isEmpty()) {
            continue;
        }
        qint64 cores = qint64(object.value(QStringLiteral("VCpuInfo"))
                                  .toObject()
                                  .value(QStringLiteral("DefaultVCpus"))
                                  .toDouble());
        if (cores <= 0) {
            cores = qint64(object.value(QStringLiteral("vcpu")).toDouble());
        }
        qint64 mib = qint64(object.value(QStringLiteral("MemoryInfo"))
                                .toObject()
                                .value(QStringLiteral("SizeInMiB"))
                                .toDouble());
        if (mib <= 0) {
            mib = qint64(object.value(QStringLiteral("mem")).toDouble());
        }
        QStringList parts;
        if (cores > 0) {
            parts.append(QStringLiteral("%1 vCPU").arg(cores));
        }
        if (mib > 0) {
            const double gib = double(mib) / 1024.0;
            if (qFuzzyCompare(gib, double(qRound(gib)))) {
                parts.append(QStringLiteral("%1 GiB").arg(qRound(gib)));
            } else {
                parts.append(QStringLiteral("%1 GiB").arg(gib, 0, 'f', 1));
            }
        }
        if (!parts.isEmpty()) {
            specs.insert(type, parts.join(QStringLiteral(", ")));
        }
    }
    return specs;
}

QHash<QString, QString> parseAwsVolumeSpecs(const QByteArray &body) {
    QHash<QString, QString> specs;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    QJsonArray volumes;
    if (document.isArray()) {
        volumes = document.array();
    } else if (document.isObject()) {
        volumes =
            document.object().value(QStringLiteral("Volumes")).toArray();
    }
    for (const QJsonValue &value : volumes) {
        const QJsonObject object = value.toObject();
        QString id = field(object, "VolumeId");
        if (id.isEmpty()) {
            id = field(object, "id");
        }
        if (id.isEmpty()) {
            continue;
        }
        qint64 size = qint64(object.value(QStringLiteral("Size")).toDouble());
        if (size <= 0) {
            size = qint64(object.value(QStringLiteral("size")).toDouble());
        }
        QString type = field(object, "VolumeType");
        if (type.isEmpty()) {
            type = field(object, "type");
        }
        qint64 iops = qint64(object.value(QStringLiteral("Iops")).toDouble());
        if (iops <= 0) {
            iops = qint64(object.value(QStringLiteral("iops")).toDouble());
        }
        QStringList parts;
        if (size > 0) {
            parts.append(QStringLiteral("%1 GiB").arg(size));
        }
        if (!type.isEmpty()) {
            parts.append(type);
        }
        if (iops > 0) {
            parts.append(QStringLiteral("%1 IOPS").arg(iops));
        }
        if (!parts.isEmpty()) {
            specs.insert(id, parts.join(QLatin1Char(' ')));
        }
    }
    return specs;
}

QString fleetInstanceDetailText(const CloudFleetInstance &row) {
    QStringList lines;
    const auto add = [&lines](const QString &label, const QString &value) {
        if (value.isEmpty()) {
            return;
        }
        lines.append(QStringLiteral("%1 %2").arg(label.leftJustified(10), value));
    };
    const auto dash = [](const QString &value) {
        return value.isEmpty() ? QStringLiteral("--") : value;
    };

    add(QStringLiteral("instance"), dash(row.instanceId));
    add(QStringLiteral("name"), row.name);
    QString type = dash(row.instanceType);
    if (!row.typeSpec.isEmpty()) {
        type += QStringLiteral("  (%1)").arg(row.typeSpec);
    }
    add(QStringLiteral("type"), type);
    QString where = dash(row.region);
    if (!row.zone.isEmpty() && row.zone != row.region) {
        where += QStringLiteral("  %1").arg(row.zone);
    }
    add(QStringLiteral("region"), where);
    QString state = dash(row.state);
    if (row.spot) {
        state += QStringLiteral("  (spot)");
    } else if (!row.instanceLifecycle.isEmpty()) {
        state += QStringLiteral("  (%1)").arg(row.instanceLifecycle);
    }
    add(QStringLiteral("state"), state);
    if (row.launchedAtMs > 0) {
        add(QStringLiteral("launched"),
            qtcompat::fromMSecsSinceEpochUtc(row.launchedAtMs)
                .toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")));
    }
    if (row.ageSec >= 0) {
        add(QStringLiteral("age"), cloudUptimeText(row.ageSec));
    }
    lines.append(QString());

    add(QStringLiteral("image"), row.imageId);
    add(QStringLiteral("os"), row.imageName);
    QString platform;
    for (const QString &part :
         {row.platformDetails, row.architecture, row.virtualizationType}) {
        if (!part.isEmpty()) {
            platform += (platform.isEmpty() ? QString() : QStringLiteral("  ")) +
                        part;
        }
    }
    add(QStringLiteral("platform"), platform);
    if (!row.rootDeviceType.isEmpty() || !row.rootVolume.isEmpty()) {
        QString root = row.rootDevice;
        if (!row.rootVolumeId.isEmpty() && !root.isEmpty()) {
            root += QStringLiteral("  ");
        }
        root += row.rootVolume;
        if (!row.rootDeviceType.isEmpty()) {
            root += root.isEmpty() ? QStringLiteral("ebs")
                                   : QStringLiteral(" %1").arg(row.rootDeviceType);
        }
        add(QStringLiteral("root"), root.simplified());
    }
    add(QStringLiteral("reason"), row.stateReason);
    lines.append(QString());

    add(QStringLiteral("key"), row.keyName);
    add(QStringLiteral("iam"),
        row.iamProfileName.isEmpty() ? row.iamProfile : row.iamProfileName);
    add(QStringLiteral("security"), row.securityGroups.join(QStringLiteral(", ")));
    lines.append(QString());

    add(QStringLiteral("vpc"), row.vpcId);
    add(QStringLiteral("subnet"), row.subnetId);
    add(QStringLiteral("private"), row.privateIp);
    add(QStringLiteral("public"), row.publicIp);
    add(QStringLiteral("dns"), row.privateDns);
    add(QStringLiteral("public dns"), row.publicDns);
    lines.append(QString());

    add(QStringLiteral("tenancy"), row.tenancy);
    add(QStringLiteral("monitoring"), row.monitoring);
    add(QStringLiteral("imds"), row.httpTokens);
    add(QStringLiteral("credit"), row.creditSpec);
    add(QStringLiteral("tags"), row.tags.join(QStringLiteral(", ")));

    while (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    if (lines.isEmpty()) {
        return QStringLiteral("-- no detail reported for this instance");
    }
    return lines.join(QLatin1Char('\n'));
}

QString fleetSummaryText(const CloudFleetStatus &fleet) {
    if (!fleet.attempted && !fleet.note.isEmpty()) {
        return fleet.note;
    }
    if (fleet.rows.isEmpty()) {
        // A run in progress (and an account that really has no instances)
        // still names the account: "naming the account ..." alone told the
        // reader nothing about which one.
        QStringList lines;
        lines.append(fleet.note.isEmpty() ? fleetEmptyText(fleet) : fleet.note);
        if (!fleet.account.isEmpty()) {
            QString line = QStringLiteral("account   %1").arg(fleet.account);
            if (!fleet.identitySource.isEmpty()) {
                line += QStringLiteral(" (%1)").arg(fleet.identitySource);
            }
            lines.append(line);
        }
        return lines.join(QLatin1Char('\n'));
    }
    const int total = fleet.rows.size() + fleet.dropped;
    QString first = QStringLiteral("%1 fleet   %2 instance%3   %4 running")
                        .arg(cloudProviderLabel(fleet.provider))
                        .arg(total)
                        .arg(total == 1 ? QString() : QStringLiteral("s"))
                        .arg(fleet.running);
    first += QStringLiteral("   %1")
                 .arg(fleet.monthlyUsd >= 0.0
                          ? QStringLiteral("%1/month").arg(
                                formatCostMoney(fleet.monthlyUsd))
                          : QStringLiteral("no priced rows"));
    if (fleet.unpriced > 0) {
        first += QStringLiteral(" (%1 unpriced)").arg(fleet.unpriced);
    }
    if (!fleet.scope.isEmpty()) {
        first += QStringLiteral("   %1").arg(fleet.scope);
    }
    QStringList lines{first};
    if (!fleet.account.isEmpty()) {
        QString line = QStringLiteral("account   %1").arg(fleet.account);
        if (!fleet.identitySource.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(fleet.identitySource);
        }
        lines.append(line);
    }
    if (fleet.selfIndex >= 0 &&
        fleet.selfIndex < fleet.rows.size()) {
        const CloudFleetInstance &self = fleet.rows.at(fleet.selfIndex);
        QString line = QStringLiteral("self      %1  %2  age %3")
                           .arg(self.name, self.state,
                                cloudUptimeText(self.ageSec));
        if (self.launchedAtMs > 0) {
            line += QStringLiteral("  launched %1")
                        .arg(qtcompat::fromMSecsSinceEpochUtc(
                                 self.launchedAtMs)
                                 .toString(QStringLiteral("yyyy-MM-dd")));
        }
        lines.append(line);
    } else {
        lines.append(QStringLiteral(
            "self      not in this listing - age unavailable (the provider "
            "did not report this instance)"));
    }
    if (fleet.regionListRefused) {
        QString line = QStringLiteral("regions   describe-regions was refused - "
                                      "listed %1 only")
                           .arg(fleet.scope.isEmpty()
                                    ? QStringLiteral("the home region")
                                    : fleet.scope);
        if (!fleet.regionListError.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(fleet.regionListError);
        }
        lines.append(line);
    } else if (!fleet.regionFailures.isEmpty()) {        QString line = QStringLiteral("regions   %1 of %2 did not answer - %3")
                           .arg(fleet.regionFailures.size())
                           .arg(fleet.regions.size())
                           .arg(fleet.regionFailures.join(
                               QStringLiteral(", ")));
        if (!fleet.regionError.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(fleet.regionError);
        }
        lines.append(line);
    }
    if (!fleet.note.isEmpty()) {
        lines.append(QStringLiteral("note      %1").arg(fleet.note));
    }
    if (fleet.dropped > 0) {
        lines.append(QStringLiteral("showing   first %1 of %2 rows")
                         .arg(fleet.rows.size())
                         .arg(total));
    }
    if (!fleet.source.isEmpty()) {
        lines.append(QStringLiteral("source    %1").arg(fleet.source));
    }
    if (fleet.fetchedAtMs > 0) {
        const qint64 ageSec = fleetAgeSeconds(
            fleet.fetchedAtMs, QDateTime::currentMSecsSinceEpoch());
        if (ageSec >= 0) {
            lines.append(QStringLiteral("listed    %1 ago")
                             .arg(cloudUptimeText(ageSec)));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString fleetTableNote() {
    return QStringLiteral(
        "* this instance - age from the provider's launch time - $/MO counts "
        "running rows only");
}

QString fleetDisabledText() {
    return QStringLiteral(
        "fleet: inventory disabled ([cloud] inventory=false) - no cloud "
        "CLI is run");
}

QString fleetNoProviderText() {
    return QStringLiteral(
        "fleet: no cloud instance identified - the account inventory needs "
        "the metadata service to name the provider");
}

QString fleetCliMissingText(const QString &cli) {
    return QStringLiteral(
               "fleet: %1 not on PATH - install it to list the account (the "
               "metadata read needs no credentials, the inventory does)")
        .arg(cli);
}

QString fleetTimeoutText(const QString &cli, int seconds) {
    return QStringLiteral(
               "fleet: %1 did not answer within %2 s - the inventory runs "
               "again on the next probe")
        .arg(cli)
        .arg(seconds);
}

QString fleetEmptyText(const CloudFleetStatus &fleet) {
    const QString where = fleet.scope.isEmpty()
                              ? QStringLiteral("this account")
                              : fleet.scope;
    return QStringLiteral(
               "fleet: %1 returned 0 instances - %2 has none")
        .arg(fleet.cliName.isEmpty()
                 ? cloudProviderName(fleet.provider)
                 : fleet.cliName,
             where);
}

QString fleetFailureText(const QString &cli, int exitCode,
                         const QString &firstErrorLine) {
    QString text = QStringLiteral("fleet: %1 exited %2").arg(cli).arg(exitCode);
    if (!firstErrorLine.isEmpty()) {
        text += QStringLiteral(" - %1").arg(firstErrorLine.left(160));
    }
    return text;
}

QString cloudFleetCredentialHint(CloudProvider provider) {
    switch (provider) {
    case CloudProvider::Aws:
        return QStringLiteral("aws configure");
    case CloudProvider::Azure:
        return QStringLiteral("az login");
    case CloudProvider::Gcp:
        return QStringLiteral("gcloud auth login");
    case CloudProvider::Oci:
        return QStringLiteral("oci setup config");
    case CloudProvider::None:
        break;
    }
    return QString();
}

QString fleetLoginExpiredText(CloudProvider provider) {
    QString command;
    switch (provider) {
    case CloudProvider::Aws:
        command = QStringLiteral("aws login");
        break;
    case CloudProvider::Azure:
        command = QStringLiteral("az login");
        break;
    case CloudProvider::Gcp:
        command = QStringLiteral("gcloud auth login");
        break;
    case CloudProvider::Oci:
        command = QStringLiteral("oci session authenticate");
        break;
    case CloudProvider::None:
        break;
    }
    return QStringLiteral(
               "fleet: the %1 login session expired - run `%2`; the listing "
               "below is the last known state")
        .arg(cloudProviderName(provider), command);
}

QString fleetCredentialsText(CloudProvider provider) {
    return QStringLiteral(
               "fleet: %1 has no credentials here - run `%2` (Crowsnest never "
               "reads or stores them)")
        .arg(cloudProviderName(provider), cloudFleetCredentialHint(provider));
}

bool cloudFleetLoginExpired(CloudProvider provider,
                            const QString &stderrText) {
    const QString text = stderrText.toLower();
    if (text.isEmpty()) {
        return false;
    }
    if (provider != CloudProvider::Aws) {
        return false;
    }
    // `aws login` issues temporary credentials plus a refresh token; when the
    // token lapses every call answers 255 with this sentence.
    return text.contains(QLatin1String("session has expired")) ||
           text.contains(QLatin1String("please reauthenticate")) ||
           text.contains(QLatin1String("aws login")) ||
           text.contains(QLatin1String("expiredtoken")) ||
           text.contains(QLatin1String("security token included in the "
                                       "request is expired")) ||
           (text.contains(QLatin1String("refresh token")) &&
            text.contains(QLatin1String("expired"))) ||
           text.contains(QLatin1String("token has expired and refresh "
                                       "failed"));
}

bool cloudFleetNeedsCredentials(CloudProvider provider,
                                const QString &stderrText) {
    const QString text = stderrText.toLower();
    if (text.isEmpty()) {
        return false;
    }
    switch (provider) {
    case CloudProvider::Aws:
        return text.contains(QLatin1String("unable to locate credentials")) ||
               text.contains(QLatin1String("nocredentials")) ||
               text.contains(QLatin1String("no credentials")) ||
               text.contains(QLatin1String("security token included in the "
                                           "request is invalid")) ||
               text.contains(QLatin1String("could not be found")) ||
               text.contains(QLatin1String("session has expired")) ||
               text.contains(QLatin1String("please reauthenticate")) ||
               text.contains(QLatin1String("aws login")) ||
               text.contains(QLatin1String("expiredtoken")) ||
               text.contains(QLatin1String("security token included in the "
                                           "request is expired")) ||
               (text.contains(QLatin1String("refresh token")) &&
                text.contains(QLatin1String("expired"))) ||
               text.contains(QLatin1String("token has expired and refresh "
                                           "failed"));
    case CloudProvider::Azure:
        return text.contains(QLatin1String("please run 'az login'")) ||
               text.contains(QLatin1String("please run az login")) ||
               text.contains(QLatin1String("aadsts")) ||
               text.contains(QLatin1String("no subscriptions found")) ||
               text.contains(QLatin1String("not logged in"));
    case CloudProvider::Gcp:
        return text.contains(QLatin1String("you do not currently have an "
                                           "active account selected")) ||
               text.contains(QLatin1String("gcloud auth login")) ||
               text.contains(QLatin1String("application default credentials"));
    case CloudProvider::Oci:
        return text.contains(QLatin1String("could not find config file")) ||
               text.contains(QLatin1String("configfilenotfound")) ||
               text.contains(QLatin1String("no credentials")) ||
               text.contains(QLatin1String("run `oci setup config`"));
    case CloudProvider::None:
        break;
    }
    return false;
}
