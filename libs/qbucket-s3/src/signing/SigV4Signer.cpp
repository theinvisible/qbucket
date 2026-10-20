#include "signing/SigV4Signer.h"

#include "http/UriEncoding.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <algorithm>
#include <map>

namespace qbucket::s3 {
namespace {

constexpr char kAlgorithm[] = "AWS4-HMAC-SHA256";

QByteArray hmac(const QByteArray& key, const QByteArray& data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

// Trim, and collapse runs of whitespace (incl. folded continuation lines)
// into one space — the SigV4 header value rule.
QByteArray normalizeHeaderValue(const QByteArray& value)
{
    QByteArray out;
    out.reserve(value.size());
    bool pendingSpace = false;
    for (const char c : value) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            out.append(' ');
            pendingSpace = false;
        }
        out.append(c);
    }
    return out;
}

QByteArray dateStamp(const QDateTime& time)
{
    return time.toUTC().toString(QStringLiteral("yyyyMMdd")).toLatin1();
}

} // namespace

QByteArray SigV4Signer::amzDate(const QDateTime& time)
{
    return time.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

QByteArray SigV4Signer::sha256Hex(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray SigV4Signer::canonicalHeaders(const HeaderList& headers, QByteArray* signedHeaders)
{
    // Lower-case names, sorted; repeated headers joined with ',' in order.
    std::map<QByteArray, QByteArray> merged;
    for (const auto& [name, value] : headers) {
        QByteArray& slot = merged[name.trimmed().toLower()];
        if (!slot.isNull())
            slot += ',';
        slot += normalizeHeaderValue(value);
        if (slot.isNull())
            slot = QByteArray("");
    }
    QByteArray canonical;
    QByteArray names;
    for (const auto& [name, value] : merged) {
        canonical += name + ':' + value + '\n';
        if (!names.isEmpty())
            names += ';';
        names += name;
    }
    if (signedHeaders)
        *signedHeaders = names;
    return canonical;
}

QByteArray SigV4Signer::canonicalRequest(const SigningInput& input)
{
    QByteArray signedHeaders;
    const QByteArray headers = canonicalHeaders(input.headers, &signedHeaders);
    return input.method + '\n'
         + input.canonicalUri + '\n'
         + input.canonicalQuery + '\n'
         + headers + '\n'
         + signedHeaders + '\n'
         + input.payloadHash;
}

QByteArray SigV4Signer::credentialScope(const SigningContext& ctx)
{
    return dateStamp(ctx.time) + '/' + ctx.region + '/' + ctx.service + "/aws4_request";
}

QByteArray SigV4Signer::stringToSign(const QByteArray& canonicalRequest, const SigningContext& ctx)
{
    return QByteArray(kAlgorithm) + '\n'
         + amzDate(ctx.time) + '\n'
         + credentialScope(ctx) + '\n'
         + sha256Hex(canonicalRequest);
}

QByteArray SigV4Signer::signingKey(const SigningContext& ctx)
{
    const QByteArray kSecret = "AWS4" + ctx.credentials.secretAccessKey.toUtf8();
    const QByteArray kDate = hmac(kSecret, dateStamp(ctx.time));
    const QByteArray kRegion = hmac(kDate, ctx.region);
    const QByteArray kService = hmac(kRegion, ctx.service);
    return hmac(kService, "aws4_request");
}

SigningResult SigV4Signer::signHeaders(const SigningInput& input, const SigningContext& ctx,
                                       bool addContentSha256)
{
    SigningResult result;
    result.addedHeaders.append({"x-amz-date", amzDate(ctx.time)});
    if (!ctx.credentials.sessionToken.isEmpty())
        result.addedHeaders.append({"x-amz-security-token", ctx.credentials.sessionToken.toUtf8()});
    if (addContentSha256)
        result.addedHeaders.append({"x-amz-content-sha256", input.payloadHash});

    SigningInput signedInput = input;
    signedInput.headers += result.addedHeaders;

    QByteArray signedHeaders;
    (void)canonicalHeaders(signedInput.headers, &signedHeaders);

    result.canonicalRequest = canonicalRequest(signedInput);
    result.stringToSign = stringToSign(result.canonicalRequest, ctx);
    result.signature = hmac(signingKey(ctx), result.stringToSign).toHex();

    const QByteArray authorization = QByteArray(kAlgorithm)
        + " Credential=" + ctx.credentials.accessKeyId.toUtf8() + '/' + credentialScope(ctx)
        + ", SignedHeaders=" + signedHeaders
        + ", Signature=" + result.signature;
    result.addedHeaders.append({"Authorization", authorization});
    return result;
}

SigningResult SigV4Signer::presign(const SigningInput& input, const SigningContext& ctx,
                                   int expiresSeconds)
{
    QByteArray signedHeaders;
    (void)canonicalHeaders(input.headers, &signedHeaders);

    // Merge the auth parameters into the (already encoded) query and re-sort.
    QList<QPair<QByteArray, QByteArray>> params;
    if (!input.canonicalQuery.isEmpty()) {
        for (const QByteArray& part : input.canonicalQuery.split('&')) {
            const qsizetype eq = part.indexOf('=');
            params.append(eq < 0 ? qMakePair(part, QByteArray())
                                 : qMakePair(part.left(eq), part.mid(eq + 1)));
        }
    }
    const auto add = [&params](const char* name, const QByteArray& raw) {
        params.append({QByteArray(name), uriEncode(raw, SlashMode::Encode)});
    };
    add("X-Amz-Algorithm", kAlgorithm);
    add("X-Amz-Credential", ctx.credentials.accessKeyId.toUtf8() + '/' + credentialScope(ctx));
    add("X-Amz-Date", amzDate(ctx.time));
    add("X-Amz-Expires", QByteArray::number(expiresSeconds));
    if (!ctx.credentials.sessionToken.isEmpty())
        add("X-Amz-Security-Token", ctx.credentials.sessionToken.toUtf8());
    add("X-Amz-SignedHeaders", signedHeaders);
    std::sort(params.begin(), params.end());

    QByteArray query;
    for (const auto& [name, value] : params) {
        if (!query.isEmpty())
            query += '&';
        query += name + '=' + value;
    }

    SigningInput signedInput = input;
    signedInput.canonicalQuery = query;

    SigningResult result;
    result.canonicalRequest = canonicalRequest(signedInput);
    result.stringToSign = stringToSign(result.canonicalRequest, ctx);
    result.signature = hmac(signingKey(ctx), result.stringToSign).toHex();
    result.presignedQuery = query + "&X-Amz-Signature=" + result.signature;
    return result;
}

} // namespace qbucket::s3
