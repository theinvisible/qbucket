// s3ls — list buckets and objects with qbucket-s3.
//
//   export AWS_ACCESS_KEY_ID=… AWS_SECRET_ACCESS_KEY=…
//   s3ls --endpoint https://fsn1.your-objectstorage.com --region fsn1 buckets
//   s3ls --endpoint http://127.0.0.1:7480 --region us-east-1 ls my-bucket photos/
//   s3ls … ls my-bucket --recursive
//
// Shows the intended use of the public API: a Session on its own worker
// thread, callbacks with a context object, ListObjectsPager for paging.

#include <qbucket/s3/ListObjectsPager.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Version.h>
#include <qbucket/s3/providers/GenericS3Profile.h>

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
        "AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption endpointOpt(QStringLiteral("endpoint"),
                                         QStringLiteral("S3 endpoint URL."), QStringLiteral("url"));
    const QCommandLineOption regionOpt(QStringLiteral("region"), QStringLiteral("Signing region."),
                                       QStringLiteral("region"), QStringLiteral("us-east-1"));
    const QCommandLineOption virtualOpt(QStringLiteral("virtual-hosted"),
                                        QStringLiteral("Bucket in the host name (default: path-style)."));
    const QCommandLineOption recursiveOpt({QStringLiteral("r"), QStringLiteral("recursive")},
                                          QStringLiteral("List without delimiter."));
    parser.addOptions({endpointOpt, regionOpt, virtualOpt, recursiveOpt});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("buckets | ls <bucket> [prefix]"));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty() || !parser.isSet(endpointOpt))
        parser.showHelp(2);

    SessionOptions options;
    options.endpoint.url = QUrl(parser.value(endpointOpt));
    options.endpoint.region = parser.value(regionOpt);
    options.endpoint.addressing = parser.isSet(virtualOpt) ? AddressingStyle::VirtualHosted
                                                           : AddressingStyle::Path;
    options.profile = std::make_shared<GenericS3Profile>();
    options.credentials = std::make_shared<EnvCredentialsProvider>();
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
    } else {
        parser.showHelp(2);
    }
    return app.exec();
}
