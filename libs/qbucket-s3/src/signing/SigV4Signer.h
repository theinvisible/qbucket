#pragma once

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>

namespace qbucket::s3 {

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

// What SigV4 needs to know about one request. The canonical URI / query come
// pre-built from S3Uri, so signing and sending use the very same bytes.
struct SigningInput {
    QByteArray method;
    QByteArray canonicalUri;
    QByteArray canonicalQuery;   // encoded + sorted, without the X-Amz-* auth params
    HeaderList headers;          // exactly the headers to sign (name case and order free)
    QByteArray payloadHash;      // lower-case hex SHA-256, or "UNSIGNED-PAYLOAD"
};

struct SigningContext {
    Credentials credentials;
    QByteArray region;
    QByteArray service = "s3";
    QDateTime time;              // UTC; already corrected for clock skew
};

// The intermediate steps are kept for diagnostics (SignatureDoesNotMatch
// reports) and tests.
struct SigningResult {
    HeaderList addedHeaders;     // header mode: x-amz-date, [x-amz-security-token],
                                 // [x-amz-content-sha256], Authorization
    QByteArray presignedQuery;   // query mode: full query string incl. X-Amz-Signature
    QByteArray canonicalRequest;
    QByteArray stringToSign;
    QByteArray signature;
};

// AWS Signature Version 4 — pure functions, thread-safe.
//
// Only the headers passed in are signed. The S3 client passes host, x-amz-*,
// content-md5, content-type and range — never headers the network stack adds
// on its own (Connection, User-Agent, Accept-Encoding, …).
class QBUCKET_S3_EXPORT SigV4Signer {
public:
    static constexpr char kUnsignedPayload[] = "UNSIGNED-PAYLOAD";
    static constexpr char kEmptyPayloadHash[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    // Header mode. With addContentSha256 (always for S3) the
    // x-amz-content-sha256 header is added and signed.
    [[nodiscard]] static SigningResult signHeaders(const SigningInput& input,
                                                   const SigningContext& ctx,
                                                   bool addContentSha256);

    // Query mode (presigned URL). The session token, if any, is part of the
    // signed query, as S3 requires.
    [[nodiscard]] static SigningResult presign(const SigningInput& input,
                                               const SigningContext& ctx, int expiresSeconds);

    // --- building blocks (public for tests) ---
    [[nodiscard]] static QByteArray canonicalHeaders(const HeaderList& headers,
                                                     QByteArray* signedHeaders);
    [[nodiscard]] static QByteArray canonicalRequest(const SigningInput& input);
    [[nodiscard]] static QByteArray credentialScope(const SigningContext& ctx);
    [[nodiscard]] static QByteArray stringToSign(const QByteArray& canonicalRequest,
                                                 const SigningContext& ctx);
    [[nodiscard]] static QByteArray signingKey(const SigningContext& ctx);
    [[nodiscard]] static QByteArray amzDate(const QDateTime& time);   // 20130524T000000Z
    [[nodiscard]] static QByteArray sha256Hex(QByteArrayView data);
};

} // namespace qbucket::s3
