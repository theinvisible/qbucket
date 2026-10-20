#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/transfer/Export.h>

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace qbucket::s3::transfer {

enum class Direction : quint8 { Upload, Download, Copy };

// What a transfer needs to continue after a restart. Never holds secrets:
// the account is the application's id, credentials come from its
// CredentialsProvider.
struct TransferRecord {
    QString id;                 // [A-Za-z0-9_-]+, unique per store
    Direction direction = Direction::Upload;
    QString account;            // the application's account id
    QString bucket;
    QString key;
    QString versionId;          // downloads / copies of a version
    QString uploadId;           // multipart uploads and copies
    QString localPath;          // upload source, download target (.qbpart)
    QString finalPath;          // download: the name after verification
    QString sourceBucket;       // copies
    QString sourceKey;
    qint64 size = 0;
    QDateTime localModified;    // upload source mtime when it started (UTC)
    QString etag;               // download / copy source ETag (If-Match)
    qint64 partSize = 0;
    int partCount = 0;
    // Uploads: what the object is created with / completed under.
    QString contentType;
    QString storageClass;
    QMap<QString, QString> metadata;
    bool ifNoneMatch = false;
    // Downloads: replace an existing target file.
    bool overwrite = false;
    QJsonObject extra;          // free for the application
};

// One finished part, as recorded when it completed.
struct PartRecord {
    int partNumber = 0;
    qint64 offset = 0;
    qint64 length = 0;
    QString etag;               // server ETag (uploads / copies)
    QByteArray md5;             // raw MD5 of the bytes
};

struct ResumeState {
    TransferRecord record;
    QList<PartRecord> parts;    // by part number, ascending; the latest record per part wins
};

// Persistence for resumable transfers (PLAN.md §3.0 inversion point).
// Implementations must be thread-safe: transfers call it from a worker.
class QBUCKET_S3_TRANSFER_EXPORT ResumeStore {
public:
    virtual ~ResumeStore();

    // Starts (or restarts) a record; earlier parts of the same id are dropped.
    virtual Result<void> begin(const TransferRecord& record) = 0;
    // Replaces the record's header, keeping its parts (e.g. the upload id).
    virtual Result<void> update(const TransferRecord& record) = 0;
    // Appends a finished part; durable once it returns (survives a crash of
    // the process).
    virtual Result<void> addPart(const QString& id, const PartRecord& part) = 0;
    virtual Result<ResumeState> load(const QString& id) const = 0;
    [[nodiscard]] virtual QStringList ids() const = 0;
    virtual Result<void> remove(const QString& id) = 0;

protected:
    ResumeStore() = default;
};

[[nodiscard]] QBUCKET_S3_TRANSFER_EXPORT bool isValidTransferId(const QString& id);

// Transfers write to their ResumeStore on a background thread, in order.
// Waits until those writes are done (e.g. before the application exits);
// false on timeout.
QBUCKET_S3_TRANSFER_EXPORT bool flushResumeWrites(int timeoutMs);

} // namespace qbucket::s3::transfer
