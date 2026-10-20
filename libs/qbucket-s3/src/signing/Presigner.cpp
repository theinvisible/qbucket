#include <qbucket/s3/Presigner.h>

#include "http/S3Uri.h"
#include "signing/SigV4Signer.h"

namespace qbucket::s3 {

Result<QUrl> Presigner::presign(const Endpoint& endpoint, AddressingStyle style,
                                const Credentials& credentials, const PresignRequest& request,
                                const QDateTime& now)
{
    if (request.expiresSeconds < 1 || request.expiresSeconds > kMaxExpirySeconds) {
        return Error::make(ErrorCategory::InvalidRequest,
                           QStringLiteral("presign expiry must be between 1 second and 7 days"));
    }
    if (!credentials.isValid())
        return Error::make(ErrorCategory::Credentials, QStringLiteral("credentials are incomplete"));
    if (request.bucket.isEmpty())
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("bucket is missing"));

    QueryParams query;
    if (!request.versionId.isEmpty())
        query.append({"versionId", request.versionId.toUtf8(), false});

    const Result<S3Uri> uri = S3Uri::build(endpoint, style, request.bucket, request.key, query);
    if (!uri)
        return uri.error();

    SigningInput input;
    input.method = request.method;
    input.canonicalUri = uri->canonicalUri();
    input.canonicalQuery = uri->canonicalQuery();
    input.headers = {{"host", uri->host()}};
    input.payloadHash = SigV4Signer::kUnsignedPayload;

    SigningContext ctx;
    ctx.credentials = credentials;
    ctx.region = endpoint.region.toUtf8();
    ctx.time = now;

    const SigningResult signedResult = SigV4Signer::presign(input, ctx, request.expiresSeconds);

    const QByteArray full = endpoint.url.scheme().toLatin1() + "://" + uri->host()
                          + uri->canonicalUri() + '?' + signedResult.presignedQuery;
    QUrl url = QUrl::fromEncoded(full, QUrl::StrictMode);
    if (!url.isValid())
        return Error::make(ErrorCategory::Internal, QStringLiteral("presigned URL is invalid"));
    return url;
}

} // namespace qbucket::s3
