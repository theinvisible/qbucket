#include "controllers/Journal.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>

namespace qbucket {

Journal::Journal(QObject* parent)
    : Journal(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/journal"), parent)
{
}

Journal::Journal(const QString& dir, QObject* parent)
    : QObject(parent),
      m_dir(dir)
{
    m_io.setMaxThreadCount(1);
    m_io.setObjectName(QStringLiteral("Journal"));
}

Journal::~Journal()
{
    m_io.waitForDone();
}

QString Journal::fileFor(const QDateTime& at) const
{
    return m_dir + u'/' + at.toUTC().toString(QStringLiteral("yyyy-MM")) + QStringLiteral(".jsonl");
}

QString Journal::resultName(Result r)
{
    switch (r) {
    case Result::Ok: return QStringLiteral("ok");
    case Result::Failed: return QStringLiteral("failed");
    case Result::Skipped: return QStringLiteral("skipped");
    case Result::Unknown: return QStringLiteral("unknown");
    }
    return {};
}

QString Journal::environmentName(Environment e)
{
    switch (e) {
    case Environment::Production: return QStringLiteral("production");
    case Environment::Staging: return QStringLiteral("staging");
    case Environment::Development: return QStringLiteral("development");
    }
    return {};
}

void Journal::append(const QList<Record>& records)
{
    if (records.isEmpty())
        return;
    // Lines are built here: the records stay on this thread.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QByteArray lines;
    QString file;
    for (const Record& r : records) {
        const QDateTime at = r.at.isValid() ? r.at.toUTC() : now;
        if (file.isEmpty())
            file = fileFor(at);
        QJsonObject o{
            {QStringLiteral("at"), at.toString(Qt::ISODateWithMs)},
            {QStringLiteral("job"), r.jobId},
            {QStringLiteral("account"), r.account.toString(QUuid::WithoutBraces)},
            {QStringLiteral("accountName"), r.accountName},
            {QStringLiteral("environment"), environmentName(r.environment)},
            {QStringLiteral("bucket"), r.bucket},
            {QStringLiteral("key"), r.key},
            {QStringLiteral("op"), r.operation},
            {QStringLiteral("result"), resultName(r.result)},
        };
        if (!r.versionId.isEmpty())
            o.insert(QStringLiteral("versionId"), r.versionId);
        if (r.httpStatus > 0)
            o.insert(QStringLiteral("status"), r.httpStatus);
        if (!r.requestId.isEmpty())
            o.insert(QStringLiteral("requestId"), r.requestId);
        if (!r.message.isEmpty())
            o.insert(QStringLiteral("message"), r.message);
        if (r.bypassGovernance)
            o.insert(QStringLiteral("bypassGovernance"), true);
        lines += QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    }
    const QString dir = m_dir;
    const QPointer<Journal> self(this);
    m_io.start([this, self, dir, file, lines] {
        QString error;
        if (!QDir().mkpath(dir)) {
            error = QStringLiteral("cannot create %1").arg(dir);
        } else {
            QFile f(file);
            const bool written = f.open(QIODevice::Append) && f.write(lines) == lines.size() && f.flush();
            if (!written)
                error = f.errorString();
        }
        if (error.isEmpty())
            return;
        QMetaObject::invokeMethod(this, [self, file, error] {
            if (self)
                emit self->writeFailed(QStringLiteral("journal %1: %2").arg(file, error));
        }, Qt::QueuedConnection);
    });
}

void Journal::waitForIdle()
{
    m_io.waitForDone();
}

} // namespace qbucket
