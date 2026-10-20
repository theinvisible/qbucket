#include <qbucket/s3/transfer/Hasher.h>

#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <optional>

namespace qbucket::s3::transfer {

namespace {

constexpr qint64 kChunk = Q_INT64_C(1024) * 1024;

Error localError(const QString& message)
{
    return Error::make(ErrorCategory::LocalIo, message);
}

} // namespace

Result<RangeHash> hashRangeNow(const QString& path, qint64 offset, qint64 length, bool withSha256,
                               const std::atomic<bool>* cancel)
{
    if (offset < 0 || length < 0)
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("invalid file range"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return localError(QStringLiteral("%1: %2").arg(path, file.errorString()));
    if (!file.seek(offset))
        return localError(QStringLiteral("%1: %2").arg(path, file.errorString()));

    QCryptographicHash md5(QCryptographicHash::Md5);
    std::optional<QCryptographicHash> sha;
    if (withSha256)
        sha.emplace(QCryptographicHash::Sha256);
    QByteArray buffer(int(std::min(kChunk, std::max<qint64>(length, 1))), Qt::Uninitialized);
    qint64 left = length;
    while (left > 0) {
        if (cancel && cancel->load())
            return Error::make(ErrorCategory::Cancelled, QStringLiteral("hashing cancelled"));
        const qint64 n = file.read(buffer.data(), std::min<qint64>(left, buffer.size()));
        if (n < 0)
            return localError(QStringLiteral("%1: %2").arg(path, file.errorString()));
        if (n == 0)
            return localError(QStringLiteral("%1: the file is shorter than expected").arg(path));
        const QByteArrayView chunk(buffer.constData(), n);
        md5.addData(chunk);
        if (sha)
            sha->addData(chunk);
        left -= n;
    }
    RangeHash out;
    out.bytes = length;
    out.md5 = md5.result();
    if (sha)
        out.sha256 = sha->result();
    return out;
}

CancelFlag hashRange(const QString& path, qint64 offset, qint64 length, bool withSha256, QObject* context,
                     std::function<void(Result<RangeHash>)> callback, QThreadPool* pool)
{
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    if (!context)
        return cancel;
    // Carries the result into the context's thread and deletes itself there
    // — never deleted by anyone else, so the worker can always post to it.
    auto* relay = new QObject;
    relay->moveToThread(context->thread());
    const QPointer<QObject> guard(context);   // read on the context's thread only
    (void)QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(),
                            [path, offset, length, withSha256, cancel, relay, guard, callback = std::move(callback)] {
                                Result<RangeHash> r = hashRangeNow(path, offset, length, withSha256, cancel.get());
                                if (cancel->load())
                                    r = Error::make(ErrorCategory::Cancelled, QStringLiteral("hashing cancelled"));
                                QMetaObject::invokeMethod(
                                    relay,
                                    [relay, guard, callback, r = std::move(r)]() mutable {
                                        if (guard)
                                            callback(std::move(r));
                                        relay->deleteLater();
                                    },
                                    Qt::QueuedConnection);
                            });
    return cancel;
}

QString md5Etag(const QByteArray& rawMd5)
{
    return QLatin1Char('"') + QString::fromLatin1(rawMd5.toHex()) + QLatin1Char('"');
}

QString compositeEtag(const QList<QByteArray>& partMd5s)
{
    QCryptographicHash all(QCryptographicHash::Md5);
    for (const QByteArray& m : partMd5s)
        all.addData(m);
    return QLatin1Char('"') + QString::fromLatin1(all.result().toHex()) + QLatin1Char('-')
         + QString::number(partMd5s.size()) + QLatin1Char('"');
}

int etagPartCount(const QString& etag)
{
    QString plain = etag;
    plain.remove(QLatin1Char('"'));
    const qsizetype dash = plain.lastIndexOf(QLatin1Char('-'));
    if (dash < 0)
        return 0;
    bool ok = false;
    const int n = plain.mid(dash + 1).toInt(&ok);
    return ok && n > 0 ? n : -1;
}

bool sameEtag(const QString& a, const QString& b)
{
    QString x = a;
    QString y = b;
    x.remove(QLatin1Char('"'));
    y.remove(QLatin1Char('"'));
    return !x.isEmpty() && x.compare(y, Qt::CaseInsensitive) == 0;
}

} // namespace qbucket::s3::transfer
