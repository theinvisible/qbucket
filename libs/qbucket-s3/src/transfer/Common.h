#pragma once

#include <qbucket/s3/Error.h>

#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <QTimeZone>

// Shared by Uploader, Downloader and Copier.
namespace qbucket::s3::transfer::detail {

inline constexpr int kPartRetries = 5;   // after the Session's own attempts
inline constexpr int kRetryDelaysMs[kPartRetries] = {1000, 3000, 10000, 30000, 60000};

inline Error localError(const QString& message)
{
    return Error::make(ErrorCategory::LocalIo, message);
}

inline bool isThrottle(const Error& e)
{
    return e.httpStatus == 503 || e.code == QLatin1String("SlowDown");
}

// Worth another attempt of the same part later.
inline bool retryable(const Error& e)
{
    return e.isTransient() || e.retryable;
}

// Size + mtime of a local file, to notice changes.
struct FileStamp {
    bool exists = false;
    qint64 size = -1;
    QDateTime modified;

    static FileStamp of(const QString& path)
    {
        const QFileInfo info(path);
        FileStamp s;
        s.exists = info.exists() && info.isFile();
        s.size = info.size();
        s.modified = info.lastModified(QTimeZone::UTC);
        return s;
    }
    [[nodiscard]] bool sameAs(qint64 size_, const QDateTime& modified_) const
    {
        return exists && size == size_ && modified == modified_;
    }
};

} // namespace qbucket::s3::transfer::detail
