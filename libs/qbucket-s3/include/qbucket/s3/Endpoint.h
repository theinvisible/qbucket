#pragma once

#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

namespace qbucket::s3 {

// How the bucket name is put into request URLs.
enum class AddressingStyle : quint8 {
    Auto,           // provider profile's default; plain virtual-hosted if none
    Path,           // https://host/bucket/key
    VirtualHosted,  // https://bucket.host/key
};

// Where and how to reach an S3 service. Value type; reentrant.
struct QBUCKET_S3_EXPORT Endpoint {
    // Scheme, host and optional port, e.g. "https://fsn1.your-objectstorage.com".
    // A path component is not supported.
    QUrl url;

    // Region used for signing, e.g. "fsn1" or "us-east-1".
    QString region;

    AddressingStyle addressing = AddressingStyle::Auto;

    // SHA-256 digests of trusted (self-signed) server certificates —
    // per-account pinning instead of ever ignoring TLS errors globally.
    QList<QByteArray> pinnedCertificateSha256;

    // Empty when usable, otherwise an English explanation.
    [[nodiscard]] QString validationError() const;
    [[nodiscard]] bool isValid() const { return validationError().isEmpty(); }

    [[nodiscard]] bool isHttps() const;
};

} // namespace qbucket::s3
