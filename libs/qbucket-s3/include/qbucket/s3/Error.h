#pragma once

#include <qbucket/s3/Export.h>

#include <QString>
#include <QtGlobal>

namespace qbucket::s3 {

// What kind of failure an Error is. Callers decide on the category, never on
// the raw S3 code or HTTP status — those are kept for logs and diagnostics.
//
// Values are append-only; they may be persisted by consumers.
enum class ErrorCategory : quint8 {
    Cancelled,           // cancelled by the caller (Session::cancel, shutdown)
    Transient,           // network error, timeout, 5xx, SlowDown, RequestTimeout
    Unsupported,         // server cannot do it: 501 NotImplemented, profile-mapped,
                         // or refused locally from the capability set
    NotPermitted,        // 403 AccessDenied — the server may support it, these
                         // credentials may not use it
    Authentication,      // InvalidAccessKeyId, SignatureDoesNotMatch, ExpiredToken
    ClockSkew,           // RequestTimeTooSkewed after automatic correction failed
    NotFound,            // NoSuchBucket / NoSuchKey / NoSuchVersion / NoSuchUpload, bare 404
    NotConfigured,       // NoSuch…Configuration, NoSuchTagSet,
                         // ObjectLockConfigurationNotFoundError — feature exists, unset
    PreconditionFailed,  // 412
    Conflict,            // 409 (BucketNotEmpty, OperationAborted, …)
    Redirect,            // a region redirect that could not be resolved
    InvalidRequest,      // other 4xx, and validation failures before sending
    Protocol,            // malformed XML, unexpected root element, 200 with <Error>,
                         // listing loop, length mismatch
    Tls,                 // TLS handshake or certificate pinning failure
    Credentials,         // the CredentialsProvider failed (keychain locked, no backend)
    Internal,            // a bug in this library
    LocalIo,             // reading or writing a local file failed (transfer bodies,
                         // download targets) — never retried
};

// A failed operation. Value type; reentrant (distinct instances may be used
// from different threads).
struct QBUCKET_S3_EXPORT Error {
    ErrorCategory category = ErrorCategory::Internal;
    int httpStatus = 0;     // 0 when no HTTP response was received
    QString code;           // S3 error code, e.g. "NoSuchKey"; empty if none
    QString message;        // English, for logs; the app translates by category
    QString requestId;      // x-amz-request-id / <RequestId>
    QString hostId;         // x-amz-id-2 / <HostId>
    QString resource;       // bucket/key the request was about
    int networkError = 0;   // QNetworkReply::NetworkError, 0 if none

    // A non-idempotent request was sent completely but its response was lost:
    // the server may or may not have executed it. Never retried blindly.
    bool outcomeUnknown = false;

    // A provider profile marked this failure as worth another attempt
    // although its category isn't Transient (e.g. Hetzner's sporadic 403s).
    // Retried for idempotent requests only; the category stays as it is.
    bool retryable = false;

    // The key's current version, or the version asked for, is a delete
    // marker (x-amz-delete-marker: true on a 404 / 405). A 405 on a marker
    // version is mapped to NotFound. versionId is the marker's, if reported.
    bool deleteMarker = false;
    QString versionId;

    [[nodiscard]] bool isTransient() const noexcept
    {
        return category == ErrorCategory::Transient;
    }

    // One line for logs: "NotFound (404 NoSuchKey): The specified key does not exist."
    [[nodiscard]] QString toString() const;

    // Stable English name of a category, e.g. "NotPermitted".
    [[nodiscard]] static QString categoryName(ErrorCategory category);

    [[nodiscard]] static Error make(ErrorCategory category, const QString& message);
};

} // namespace qbucket::s3
