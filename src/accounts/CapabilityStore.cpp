#include "accounts/CapabilityStore.h"

#include "diagnostics/Logging.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace qbucket {

namespace {

const QString kServerFile = QStringLiteral("server.json");
const QString kBucketPrefix = QStringLiteral("bucket-");

bool writeJson(const QString& path, const QJsonObject& json, QString* error)
{
    QSaveFile f(path);   // atomic: a crash never leaves half a file
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

s3::Result<s3::CapabilitySet> readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return s3::Error::make(s3::ErrorCategory::Internal, f.errorString());
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (!doc.isObject())
        return s3::Error::make(s3::ErrorCategory::Protocol, parseError.errorString());
    return s3::CapabilitySet::fromJson(doc.object());
}

} // namespace

CapabilityStore::CapabilityStore(QObject* parent)
    : CapabilityStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                          + QStringLiteral("/capabilities"),
                      parent)
{
}

CapabilityStore::CapabilityStore(const QString& rootDir, QObject* parent)
    : QObject(parent),
      m_root(rootDir)
{
    m_io.setMaxThreadCount(1);
    m_io.setObjectName(QStringLiteral("CapabilityStore"));
}

CapabilityStore::~CapabilityStore()
{
    m_io.waitForDone();
}

QString CapabilityStore::fileNameFor(const s3::CapabilitySet& set)
{
    if (set.scope == s3::CapabilityScope::Server)
        return kServerFile;
    static const QRegularExpression plain(QStringLiteral("^[a-z0-9][a-z0-9.-]{0,62}$"));
    const QString name = plain.match(set.bucket).hasMatch()
                             ? set.bucket
                             : QStringLiteral("x-") + QString::fromLatin1(set.bucket.toUtf8().toHex());
    return kBucketPrefix + name + QStringLiteral(".json");
}

QString CapabilityStore::accountDir_(const QUuid& account) const
{
    return m_root + u'/' + account.toString(QUuid::WithoutBraces);
}

void CapabilityStore::load(const QUuid& account, QObject* context, LoadCallback done)
{
    const QString dir = accountDir_(account);
    const QPointer<QObject> guard(context);
    // Answered via `this` (GUI thread), which outlives every task (the
    // destructor waits); the context is only checked there.
    m_io.start([this, dir, guard, done = std::move(done)] {
        Snapshot snap;
        const QDir d(dir);
        const QStringList files = d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& file : files) {
            const bool isServer = file == kServerFile;
            if (!isServer && !file.startsWith(kBucketPrefix))
                continue;
            s3::Result<s3::CapabilitySet> r = readJson(d.filePath(file));
            const bool scopeOk = r && (isServer ? r->scope == s3::CapabilityScope::Server
                                                : r->scope == s3::CapabilityScope::Bucket && !r->bucket.isEmpty());
            if (!scopeOk) {
                snap.problems.append(QStringLiteral("%1: %2").arg(
                    d.filePath(file), r ? QStringLiteral("unexpected scope") : r.error().message));
                continue;
            }
            if (isServer)
                snap.server = std::move(*r);
            else
                snap.buckets.append(std::move(*r));
        }
        QMetaObject::invokeMethod(this, [guard, done, snap] {
            if (guard)
                done(snap);
        }, Qt::QueuedConnection);
    });
}

void CapabilityStore::save(const QUuid& account, const s3::CapabilitySet& set)
{
    const QString dir = accountDir_(account);
    const QString path = dir + u'/' + fileNameFor(set);
    const QJsonObject json = set.toJson();
    m_io.start([dir, path, json] {
        QString error;
        if (!QDir().mkpath(dir))
            error = QStringLiteral("cannot create the directory");
        else if (writeJson(path, json, &error))
            return;
        qCWarning(lcApp).noquote() << "could not save capabilities to" << path << ':' << error;
    });
}

void CapabilityStore::removeAll(const QUuid& account)
{
    const QString dir = accountDir_(account);
    m_io.start([dir] {
        QDir d(dir);
        if (d.exists() && !d.removeRecursively())
            qCWarning(lcApp).noquote() << "could not remove" << dir;
    });
}

void CapabilityStore::waitForIdle()
{
    m_io.waitForDone();
}

} // namespace qbucket
