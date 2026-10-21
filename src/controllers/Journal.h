#pragma once

#include "domain/Account.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUuid>

namespace qbucket {

// The audit journal (PLAN.md §12.4): every mutating request, one JSON line
// each, in AppDataLocation/journal/YYYY-MM.jsonl — time, account,
// environment, bucket, key, version, operation, result, HTTP status,
// x-amz-request-id. Never a secret. Appended on a background thread, in
// call order; the destructor waits for pending lines.
//
// Thread affinity: GUI thread.
class Journal : public QObject {
    Q_OBJECT

public:
    enum class Result { Ok, Failed, Skipped, Unknown };   // Unknown: outcome unclear (lost answer)
    struct Record {
        QDateTime at;          // UTC; set by append() if invalid
        QString jobId;
        QUuid account;
        QString accountName;
        Environment environment = Environment::Production;
        QString bucket;
        QString key;
        QString versionId;
        QString operation;     // s3::operationName(), e.g. "DeleteObjects"
        Result result = Result::Ok;
        int httpStatus = 0;
        QString requestId;
        QString message;       // error text or note
        bool bypassGovernance = false;   // sent with x-amz-bypass-governance-retention
    };

    explicit Journal(QObject* parent = nullptr);
    explicit Journal(const QString& dir, QObject* parent = nullptr);
    ~Journal() override;

    void append(const QList<Record>& records);
    void append(const Record& record) { append(QList<Record>{record}); }

    [[nodiscard]] QString dir() const { return m_dir; }
    [[nodiscard]] QString fileFor(const QDateTime& at) const;
    // Blocks until queued writes are done — tests and shutdown.
    void waitForIdle();

    [[nodiscard]] static QString resultName(Result r);
    [[nodiscard]] static QString environmentName(Environment e);

signals:
    // Lines could not be written (disk full, permissions); English, for the log.
    void writeFailed(const QString& message);

private:
    QString m_dir;
    QThreadPool m_io;   // one thread: lines stay in order
};

} // namespace qbucket
