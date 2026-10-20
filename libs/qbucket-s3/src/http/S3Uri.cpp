#include "http/S3Uri.h"

#include "http/UriEncoding.h"

#include <QHostAddress>

#include <algorithm>

namespace qbucket::s3 {
namespace {

bool isIpLiteralOrLocalhost(const QString& host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    return !QHostAddress(host).isNull();
}

// Host header / authority form of a host: IPv6 literals in brackets.
QByteArray authorityHost(const QString& host)
{
    if (host.contains(QLatin1Char(':')))
        return '[' + host.toLatin1() + ']';
    return host.toLatin1();
}

int defaultPort(const QUrl& url)
{
    return url.scheme() == QLatin1String("https") ? 443 : 80;
}

} // namespace

bool S3Uri::isDnsCompatibleBucketName(const QString& bucket)
{
    if (bucket.size() < 3 || bucket.size() > 63)
        return false;
    for (const QChar c : bucket) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                     || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                     || c == QLatin1Char('-') || c == QLatin1Char('.');
        if (!ok)
            return false;
    }
    const auto alnum = [](QChar c) { return c.isLetterOrNumber(); };
    if (!alnum(bucket.front()) || !alnum(bucket.back()))
        return false;
    if (bucket.contains(QLatin1String("..")) || bucket.contains(QLatin1String(".-"))
        || bucket.contains(QLatin1String("-.")))
        return false;
    // Formatted like an IPv4 address.
    if (!QHostAddress(bucket).isNull())
        return false;
    return true;
}

AddressingStyle S3Uri::effectiveStyle(AddressingStyle preferred, const QString& bucket,
                                      const QUrl& endpointUrl)
{
    if (preferred == AddressingStyle::Path || bucket.isEmpty())
        return AddressingStyle::Path;
    if (!isDnsCompatibleBucketName(bucket))
        return AddressingStyle::Path;
    if (bucket.contains(QLatin1Char('.')) && endpointUrl.scheme() == QLatin1String("https"))
        return AddressingStyle::Path;
    if (isIpLiteralOrLocalhost(endpointUrl.host()))
        return AddressingStyle::Path;
    return AddressingStyle::VirtualHosted;
}

Result<S3Uri> S3Uri::build(const Endpoint& endpoint, AddressingStyle style, const QString& bucket,
                           const QString& key, const QueryParams& query)
{
    if (const QString err = endpoint.validationError(); !err.isEmpty())
        return Error::make(ErrorCategory::InvalidRequest, err);
    if (bucket.isEmpty() && !key.isEmpty())
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("object key without a bucket"));

    S3Uri uri;
    uri.m_style = effectiveStyle(style, bucket, endpoint.url);

    // --- Host ---
    QByteArray host = authorityHost(endpoint.url.host().toLower());
    if (uri.m_style == AddressingStyle::VirtualHosted)
        host = bucket.toLatin1() + '.' + host;
    const int port = endpoint.url.port();
    if (port != -1 && port != defaultPort(endpoint.url))
        host += ':' + QByteArray::number(port);
    uri.m_host = host;

    // --- Path ---
    QByteArray path = "/";
    if (uri.m_style == AddressingStyle::Path && !bucket.isEmpty()) {
        path += uriEncodeText(bucket, SlashMode::Encode);
        if (!key.isEmpty())
            path += '/';
    }
    path += uriEncodeText(key, SlashMode::Keep);
    uri.m_path = path;

    // --- Query: sorted by encoded name, then value (SigV4 canonical order) ---
    struct Encoded {
        QByteArray name;
        QByteArray value;
        bool bare;
    };
    QList<Encoded> enc;
    enc.reserve(query.size());
    for (const QueryParam& p : query)
        enc.append({uriEncode(p.name, SlashMode::Encode), uriEncode(p.value, SlashMode::Encode), p.bare});
    std::sort(enc.begin(), enc.end(), [](const Encoded& a, const Encoded& b) {
        return a.name != b.name ? a.name < b.name : a.value < b.value;
    });
    QByteArray wire;
    QByteArray canonical;
    for (const Encoded& e : enc) {
        if (!wire.isEmpty()) {
            wire += '&';
            canonical += '&';
        }
        wire += e.name;
        if (!e.bare)
            wire += '=' + e.value;
        canonical += e.name + '=' + e.value;
    }
    uri.m_query = wire;
    uri.m_canonicalQuery = canonical;

    // --- URL --- built from the encoded form, then checked: QUrl must send
    // exactly the bytes we sign, never re-encode or normalise them.
    QByteArray full = endpoint.url.scheme().toLatin1() + "://" + host + path;
    if (!wire.isEmpty())
        full += '?' + wire;
    uri.m_url = QUrl::fromEncoded(full, QUrl::StrictMode);
    if (!uri.m_url.isValid()
        || uri.m_url.path(QUrl::FullyEncoded).toLatin1() != path
        || uri.m_url.query(QUrl::FullyEncoded).toLatin1() != wire) {
        return Error::make(ErrorCategory::Internal,
                           QStringLiteral("QUrl altered the request target: ")
                               + QString::fromLatin1(full));
    }
    return uri;
}

} // namespace qbucket::s3
