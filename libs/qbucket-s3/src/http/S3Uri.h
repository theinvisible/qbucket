#pragma once

#include <qbucket/s3/Endpoint.h>
#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

namespace qbucket::s3 {

// One query parameter, raw (not yet encoded) bytes. A "bare" parameter is a
// subresource without value, sent as "?versioning" but canonicalised as
// "versioning=" for signing.
struct QueryParam {
    QByteArray name;
    QByteArray value;
    bool bare = false;
};
using QueryParams = QList<QueryParam>;

// The exact request target of one S3 request: URL, Host header value, and the
// canonical URI / query for SigV4 — built once, so what is signed and what is
// sent cannot drift apart.
//
// Value type; reentrant.
class QBUCKET_S3_EXPORT S3Uri {
public:
    // style should already be resolved against the provider profile; Auto
    // here means "virtual-hosted where possible". An empty bucket addresses
    // the service (ListBuckets).
    [[nodiscard]] static Result<S3Uri> build(const Endpoint& endpoint, AddressingStyle style,
                                             const QString& bucket, const QString& key,
                                             const QueryParams& query = {});

    // The addressing style actually usable for this bucket: virtual-hosted
    // falls back to path-style for names that aren't DNS-compatible, for
    // dotted names over TLS (the wildcard certificate can't match), and for
    // IP-literal / localhost endpoints.
    [[nodiscard]] static AddressingStyle effectiveStyle(AddressingStyle preferred,
                                                        const QString& bucket,
                                                        const QUrl& endpointUrl);

    [[nodiscard]] static bool isDnsCompatibleBucketName(const QString& bucket);

    [[nodiscard]] const QUrl& url() const { return m_url; }
    [[nodiscard]] const QByteArray& host() const { return m_host; }               // Host header value
    [[nodiscard]] const QByteArray& canonicalUri() const { return m_path; }       // == encoded path sent
    [[nodiscard]] const QByteArray& canonicalQuery() const { return m_canonicalQuery; }
    [[nodiscard]] const QByteArray& encodedQuery() const { return m_query; }      // as sent, without '?'
    [[nodiscard]] AddressingStyle style() const { return m_style; }

private:
    QUrl m_url;
    QByteArray m_host;
    QByteArray m_path;
    QByteArray m_query;
    QByteArray m_canonicalQuery;
    AddressingStyle m_style = AddressingStyle::Path;
};

} // namespace qbucket::s3
