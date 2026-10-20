// s3cp — upload files with qbucket-s3's transfer engine, resumably.
//
//   export AWS_ACCESS_KEY_ID=… AWS_SECRET_ACCESS_KEY=…
//   s3cp --provider hetzner --region nbg1 upload big.iso my-bucket isos/big.iso
//   s3cp --provider hetzner --region nbg1 --resume-dir ~/.s3cp --id iso1 upload big.iso my-bucket isos/big.iso
//   s3cp --provider hetzner --region nbg1 --resume-dir ~/.s3cp resume iso1      (after a crash or kill)
//   s3cp --resume-dir ~/.s3cp list
//
// Shows the transfer API: a Session with transfer lanes, one
// AdaptiveConcurrency per endpoint, an Uploader, a JsonFileResumeStore.

#include <qbucket/s3/Session.h>
#include <qbucket/s3/Version.h>
#include <qbucket/s3/providers/ProviderRegistry.h>
#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/JsonFileResumeStore.h>
#include <qbucket/s3/transfer/Uploader.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextStream>

#include <memory>

using namespace qbucket::s3;
using namespace qbucket::s3::transfer;

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

QString integrityName(Integrity i)
{
    return i == Integrity::Md5Verified ? QStringLiteral("md5-verified") : QStringLiteral("size-verified");
}

QString humanBytes(qint64 n)
{
    return QLocale::c().formattedDataSize(n, 1, QLocale::DataSizeIecFormat);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("s3cp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QBUCKET_S3_VERSION_STRING));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Upload files to S3 with multipart, verification and resume. Credentials come from "
        "AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN or --aws-profile."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption providerOpt(QStringLiteral("provider"), QStringLiteral("Provider profile: hetzner, aws, garage, generic."),
                                         QStringLiteral("id"), QStringLiteral("generic"));
    const QCommandLineOption endpointOpt(QStringLiteral("endpoint"), QStringLiteral("S3 endpoint URL (default: the provider's for --region)."),
                                         QStringLiteral("url"));
    const QCommandLineOption regionOpt(QStringLiteral("region"), QStringLiteral("Region."), QStringLiteral("region"));
    const QCommandLineOption profileOpt(QStringLiteral("aws-profile"), QStringLiteral("Profile in the AWS shared credentials file."),
                                        QStringLiteral("name"));
    const QCommandLineOption pathOpt(QStringLiteral("path-style"), QStringLiteral("Bucket in the path (default: the provider's choice)."));
    const QCommandLineOption resumeDirOpt(QStringLiteral("resume-dir"),
                                          QStringLiteral("Record transfers here so they survive a crash (resume <id>)."),
                                          QStringLiteral("dir"));
    const QCommandLineOption idOpt(QStringLiteral("id"), QStringLiteral("Transfer id for --resume-dir (default: random)."),
                                   QStringLiteral("id"));
    const QCommandLineOption thresholdOpt(QStringLiteral("multipart-threshold"), QStringLiteral("Bytes from which a file is split."),
                                          QStringLiteral("bytes"));
    const QCommandLineOption partSizeOpt(QStringLiteral("part-size"), QStringLiteral("Smallest part size in bytes."),
                                         QStringLiteral("bytes"));
    const QCommandLineOption concurrencyOpt(QStringLiteral("concurrency"), QStringLiteral("Parts in parallel at most (default 16)."),
                                            QStringLiteral("n"), QStringLiteral("16"));
    const QCommandLineOption lanesOpt(QStringLiteral("lanes"), QStringLiteral("Transfer threads (default 4)."),
                                      QStringLiteral("n"), QStringLiteral("4"));
    const QCommandLineOption progressOpt(QStringLiteral("progress"), QStringLiteral("Print \"progress <done> <total>\" lines."));
    parser.addOptions({providerOpt, endpointOpt, regionOpt, profileOpt, pathOpt, resumeDirOpt, idOpt, thresholdOpt,
                       partSizeOpt, concurrencyOpt, lanesOpt, progressOpt});
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("upload <file> <bucket> <key> | resume <id> | list"));
    parser.process(app);
    const QStringList args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(2);

    std::shared_ptr<JsonFileResumeStore> store;
    if (parser.isSet(resumeDirOpt))
        store = std::make_shared<JsonFileResumeStore>(parser.value(resumeDirOpt));
    const QString& command = args.first();
    if (command == QLatin1String("list")) {
        if (!store) {
            err() << "error: list needs --resume-dir" << Qt::endl;
            return 2;
        }
        for (const QString& id : store->ids()) {
            const auto s = store->load(id);
            if (!s)
                continue;
            out() << id << ' ' << s->record.bucket << '/' << s->record.key << ' ' << s->parts.size() << '/'
                  << s->record.partCount << " parts" << Qt::endl;
        }
        return 0;
    }

    SessionOptions options;
    options.profile = ProviderRegistry::builtin().findOrGeneric(parser.value(providerOpt));
    options.endpoint.region = parser.isSet(regionOpt) ? parser.value(regionOpt)
                                                      : options.profile->regions().value(0, options.profile->signingRegion(QString()));
    options.endpoint.url = parser.isSet(endpointOpt) ? QUrl(parser.value(endpointOpt))
                                                     : options.profile->endpointFor(options.endpoint.region);
    options.endpoint.addressing = parser.isSet(pathOpt) ? AddressingStyle::Path : AddressingStyle::Auto;
    if (parser.isSet(profileOpt))
        options.credentials = std::make_shared<AwsSharedFileCredentialsProvider>(QString(), parser.value(profileOpt));
    else
        options.credentials = std::make_shared<EnvCredentialsProvider>();
    options.transferLanes = std::max(1, parser.value(lanesOpt).toInt());
    if (const QString e = options.endpoint.validationError(); !e.isEmpty()) {
        err() << "error: " << e << Qt::endl;
        return 2;
    }
    Session session(options);
    AdaptiveConcurrency::Options limits;
    limits.max = std::max(1, parser.value(concurrencyOpt).toInt());
    limits.initial = std::min(limits.initial, limits.max);
    AdaptiveConcurrency limiter(limits);

    std::unique_ptr<Uploader> uploader;
    if (command == QLatin1String("upload") && args.size() == 4) {
        UploadRequest r;
        r.localPath = args.at(1);
        r.bucket = args.at(2);
        r.key = args.at(3);
        if (parser.isSet(thresholdOpt))
            r.planner.multipartThreshold = parser.value(thresholdOpt).toLongLong();
        if (parser.isSet(partSizeOpt))
            r.planner.minPartSize = parser.value(partSizeOpt).toLongLong();
        r.transferId = parser.isSet(idOpt) ? parser.value(idOpt)
                                           : QString::number(QRandomGenerator::global()->generate64(), 16);
        uploader = std::make_unique<Uploader>(&session, r, &limiter, store);
        if (store)
            out() << "transfer id " << r.transferId << Qt::endl;
    } else if (command == QLatin1String("resume") && args.size() == 2) {
        if (!store) {
            err() << "error: resume needs --resume-dir" << Qt::endl;
            return 2;
        }
        uploader = Uploader::fromStore(&session, store, args.at(1), &limiter);
    } else {
        parser.showHelp(2);
    }

    QElapsedTimer clock;
    clock.start();
    const bool progressLines = parser.isSet(progressOpt);
    QObject::connect(uploader.get(), &Uploader::progress, &app, [&](qint64 done, qint64 total) {
        if (progressLines)
            out() << "progress " << done << ' ' << total << Qt::endl;
    });
    QObject::connect(uploader.get(), &Uploader::finished, &app, [&] {
        // The last part records must be on disk before we exit.
        flushResumeWrites(10000);
        const Result<UploadResult>& r = uploader->result();
        if (!r) {
            err() << "error: " << r.error().toString() << Qt::endl;
            QCoreApplication::exit(1);
            return;
        }
        const double secs = std::max<double>(0.001, double(clock.elapsed()) / 1000.0);
        out() << "uploaded " << uploader->request().bucket << '/' << uploader->request().key << ' ' << r->etag << ' '
              << integrityName(r->integrity) << ' ' << r->parts << " parts" << (r->resumed ? " (resumed)" : "")
              << (r->restarted ? " (restarted)" : "") << ' ' << humanBytes(r->size) << " in "
              << QString::number(secs, 'f', 1) << " s, " << humanBytes(qint64(double(r->size) / secs)) << "/s"
              << Qt::endl;
        QCoreApplication::exit(0);
    });
    uploader->start();
    return QCoreApplication::exec();
}
