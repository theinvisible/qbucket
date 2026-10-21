#include <qbucket/s3/providers/AwsProfile.h>

namespace qbucket::s3 {

QString AwsProfile::id() const
{
    return QStringLiteral("aws");
}

QString AwsProfile::displayName() const
{
    return QStringLiteral("Amazon S3");
}

QStringList AwsProfile::regions() const
{
    return {
        QStringLiteral("us-east-1"),      QStringLiteral("us-east-2"),
        QStringLiteral("us-west-1"),      QStringLiteral("us-west-2"),
        QStringLiteral("ca-central-1"),   QStringLiteral("sa-east-1"),
        QStringLiteral("eu-central-1"),   QStringLiteral("eu-central-2"),
        QStringLiteral("eu-west-1"),      QStringLiteral("eu-west-2"),
        QStringLiteral("eu-west-3"),      QStringLiteral("eu-north-1"),
        QStringLiteral("eu-south-1"),     QStringLiteral("eu-south-2"),
        QStringLiteral("ap-northeast-1"), QStringLiteral("ap-northeast-2"),
        QStringLiteral("ap-northeast-3"), QStringLiteral("ap-southeast-1"),
        QStringLiteral("ap-southeast-2"), QStringLiteral("ap-south-1"),
    };
}

QUrl AwsProfile::endpointFor(const QString& region) const
{
    const QString r = region.isEmpty() ? QStringLiteral("us-east-1") : region;
    return QUrl(QStringLiteral("https://s3.%1.amazonaws.com").arg(r));
}

AddressingStyle AwsProfile::defaultAddressing() const
{
    return AddressingStyle::VirtualHosted;
}

QStringList AwsProfile::storageClasses() const
{
    return {QStringLiteral("STANDARD"), QStringLiteral("STANDARD_IA"), QStringLiteral("ONEZONE_IA"),
            QStringLiteral("INTELLIGENT_TIERING"), QStringLiteral("GLACIER_IR"), QStringLiteral("GLACIER"),
            QStringLiteral("DEEP_ARCHIVE")};
}

CapabilitySet AwsProfile::capabilities() const
{
    CapabilitySet set = ProviderProfile::capabilities();
    const CapabilityEntry yes{CapabilityState::Supported, CapabilitySource::Profile, {}, 0, {}, false, 0};
    set.set(Feature::BatchDelete, yes);
    set.set(Feature::CrossBucketCopy, yes);
    set.set(Feature::ObjectTaggingWrite, yes);
    set.set(Feature::ObjectAclWrite, yes);
    return set;
}

} // namespace qbucket::s3
