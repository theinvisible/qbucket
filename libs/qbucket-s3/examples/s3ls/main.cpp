// s3ls — list buckets and objects with qbucket-s3.
//
//   export AWS_ACCESS_KEY_ID=… AWS_SECRET_ACCESS_KEY=…
//   s3ls --provider hetzner --region fsn1 buckets
//   s3ls --endpoint http://127.0.0.1:7480 --region us-east-1 ls my-bucket photos/
//   s3ls --provider aws --region eu-central-1 --aws-profile work ls my-bucket -r
//
// Shows the intended use of the public API: a Session on its own worker
// thread, callbacks with a context object, ListObjectsPager for paging.

#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Version.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include <memory>

using namespace qbucket::s3;

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

int fail(const Error& e)
{
    err() << "error: " << e.toString() << Qt::endl;
    return 1;
}

// Prints one listing page per callback, asks for the next until done.
void listAll(ListObjectsPager& pager, QObject* context)
{
    pager.fetchNext(context, [&pager, context](Result<ListPage> page) {
        if (!page) {
            QCoreApplication::exit(fail(page.error()));
            return;
        }
        for (const QString& prefix : page->commonPrefixes)
            out() << QStringLiteral("%1 %2").arg(QStringLiteral("PRE"), 34) .arg(prefix) << '\n';
        for (const ObjectEntry& e : page->objects) {
            out() << e.lastModified.toString(Qt::ISODate) << ' '
                  << QStringLiteral("%1").arg(e.size, 14) << ' ' << e.key << '\n';
        }
        out().flush();
        if (pager.hasMore())
            listAll(pager, context);
        else
            QCoreApplication::exit(0);
    });
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("s3ls"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QBUCKET_S3_VERSION_STRING));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "List S3 buckets and objects. Credentials come from AWS_ACCESS_KEY_ID, "
        "AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN, or with --aws-profile from "
        "the AWS shared credentials file."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption providerOpt(QStringLiteral("provider"),
                                         QStringLiteral("Provider profile: hetzner, aws, garage, generic."),
                                         QStringLiteral("id"), QStringLiteral("generic"));
    const QCommandLineOption endpointOpt(QStringLiteral("endpoint"),
                                         QStringLiteral("S3 endpoint URL (default: the provider's for --region)."),
                                         QStringLiteral("url"));
    const QCommandLineOption regionOpt(QStringLiteral("region"), QStringLiteral("Region."),
                                       QStringLiteral("region"));
    const QCommandLineOption profileOpt(QStringLiteral("aws-profile"),
                                        QStringLiteral("Profile in the AWS shared credentials file."),
                                        QStringLiteral("name"));
    const QCommandLineOption pathOpt(QStringLiteral("path-style"),
                                     QStringLiteral("Bucket in the path (default: the provider's choice)."));
    const QCommandLineOption recursiveOpt({QStringLiteral("r"), QStringLiteral("recursive")},
                                          QStringLiteral("List without delimiter."));
    parser.addOptions({providerOpt, endpointOpt, regionOpt, profileOpt, pathOpt, recursiveOpt});
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("buckets | ls <bucket> [prefix] | probe <bucket>"));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(2);

    SessionOptions options;
    options.profile = ProviderRegistry::builtin().findOrGeneric(parser.value(providerOpt));
    options.endpoint.region = parser.isSet(regionOpt)
        ? parser.value(regionOpt)
        : options.profile->regions().value(0, options.profile->signingRegion(QString()));
    options.endpoint.url = parser.isSet(endpointOpt) ? QUrl(parser.value(endpointOpt))
                                                     : options.profile->endpointFor(options.endpoint.region);
    options.endpoint.addressing = parser.isSet(pathOpt) ? AddressingStyle::Path : AddressingStyle::Auto;
    if (parser.isSet(profileOpt)) {
        options.credentials = std::make_shared<AwsSharedFileCredentialsProvider>(
            QString(), parser.value(profileOpt));
    } else {
        options.credentials = std::make_shared<EnvCredentialsProvider>();
    }
    if (const QString e = options.endpoint.validationError(); !e.isEmpty()) {
        err() << "error: " << e << Qt::endl;
        return 2;
    }
    Session session(options);

    std::unique_ptr<ListObjectsPager> pager;
    const QString command = args.first();
    if (command == QLatin1String("buckets")) {
        session.listBuckets(&app, [](Result<BucketList> list) {
            if (!list) {
                QCoreApplication::exit(fail(list.error()));
                return;
            }
            for (const BucketInfo& b : list->buckets)
                out() << b.creationDate.toString(Qt::ISODate) << ' ' << b.name << '\n';
            QCoreApplication::exit(0);
        });
    } else if (command == QLatin1String("ls") && args.size() >= 2) {
        ListObjectsParams params;
        params.bucket = args.at(1);
        params.prefix = args.value(2);
        if (!parser.isSet(recursiveOpt))
            params.delimiter = QStringLiteral("/");
        pager = std::make_unique<ListObjectsPager>(session, params);
        listAll(*pager, &app);
    } else if (command == QLatin1String("probe") && args.size() >= 2) {
        // Read-only: what can this server do for this bucket?
        session.probeBucket(args.at(1), &app, [](Result<CapabilitySet> set) {
            if (!set) {
                QCoreApplication::exit(fail(set.error()));
                return;
            }
            for (const auto& [feature, e] : set->entries()) {
                out() << QStringLiteral("%1 %2 %3 %4%5")
                             .arg(featureId(feature), -28)
                             .arg(stateName(e.state), -13)
                             .arg(e.httpStatus ? QString::number(e.httpStatus) : QStringLiteral("-"), 3)
                             .arg(e.s3Code)
                             .arg(e.configured ? QStringLiteral(" (configured)") : QString())
                      << '\n';
            }
            out() << "versioning: " << (set->bucketState.versioning.isEmpty() ? QStringLiteral("never enabled")
                                                                               : set->bucketState.versioning)
                  << ", object lock: "
                  << (set->bucketState.objectLockEnabled
                          ? (*set->bucketState.objectLockEnabled ? QStringLiteral("enabled") : QStringLiteral("off"))
                          : QStringLiteral("unknown"))
                  << "\nserver: " << set->fingerprint.serverHeader << " ("
                  << set->fingerprint.requestIdShape << ", " << set->fingerprint.hash() << ")" << Qt::endl;
            QCoreApplication::exit(0);
        });
    } else {
        parser.showHelp(2);
    }
    return app.exec();
}
