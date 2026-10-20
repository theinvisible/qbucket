#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>

// Parsers for S3 XML responses (QXmlStreamReader). Elements are matched by
// local name only — some servers omit the S3 namespace. Every parser checks
// the root element: a different root (e.g. a listing where a configuration
// was expected) is a Protocol error, never silently "empty".
//
// Pure functions; thread-safe.
namespace qbucket::s3::xml {

// Fields of an <Error> body.
struct ErrorBody {
    QString code;
    QString message;
    QString resource;
    QString requestId;
    QString hostId;
    QString region;          // AuthorizationHeaderMalformed / redirects
    QString endpoint;        // PermanentRedirect
    QDateTime serverTime;    // RequestTimeTooSkewed
};

// Local name of the document's root element; empty if the body isn't XML.
QBUCKET_S3_EXPORT QString rootElementName(const QByteArray& body);

// nullopt unless the body is an <Error> document.
QBUCKET_S3_EXPORT std::optional<ErrorBody> parseErrorBody(const QByteArray& body);

// ListAllMyBucketsResult. *continuationToken receives the token for the next
// page (servers supporting paginated ListBuckets), empty when done.
QBUCKET_S3_EXPORT Result<BucketList> parseListBuckets(const QByteArray& body,
                                                      QString* continuationToken);

// ListBucketResult of ListObjectsV2. Keys/prefixes are decoded when the
// response says <EncodingType>url</EncodingType> (wherever that element
// appears); plusIsSpace is a provider quirk.
QBUCKET_S3_EXPORT Result<ListPage> parseListObjectsV2(const QByteArray& body, bool plusIsSpace);

// ListBucketResult of the V1 API. nextContinuationToken carries the marker
// for the next page (NextMarker, or the last key/prefix when absent).
QBUCKET_S3_EXPORT Result<ListPage> parseListObjectsV1(const QByteArray& body, bool plusIsSpace);

QBUCKET_S3_EXPORT Result<DeleteObjectsResult> parseDeleteResult(const QByteArray& body);

// <CopyObjectResult> of a successful copy (ETag, LastModified).
QBUCKET_S3_EXPORT Result<CopyObjectResult> parseCopyObjectResult(const QByteArray& body);

// <InitiateMultipartUploadResult> (Bucket, Key, UploadId).
QBUCKET_S3_EXPORT Result<CreateMultipartUploadResult> parseInitiateMultipartUpload(const QByteArray& body);

// <CompleteMultipartUploadResult> (Location, ETag).
QBUCKET_S3_EXPORT Result<CompleteMultipartUploadResult> parseCompleteMultipartUpload(const QByteArray& body);

// <ListPartsResult>. A truncated page must name the next marker.
QBUCKET_S3_EXPORT Result<ListPartsPage> parseListParts(const QByteArray& body);

// <ListMultipartUploadsResult>; keys decoded for encoding-type=url.
QBUCKET_S3_EXPORT Result<ListMultipartUploadsPage> parseListMultipartUploads(const QByteArray& body,
                                                                             bool plusIsSpace);

// <CopyPartResult> of UploadPartCopy (ETag, LastModified).
QBUCKET_S3_EXPORT Result<UploadPartCopyResult> parseCopyPartResult(const QByteArray& body);

// Raw LocationConstraint value ("" for us-east-1, legacy "EU", …). Mapping
// to a signing region is the provider profile's job.
QBUCKET_S3_EXPORT Result<QString> parseLocationConstraint(const QByteArray& body);

// S3 timestamps: "2009-10-12T17:50:30.000Z" (listings) — UTC.
QBUCKET_S3_EXPORT QDateTime parseIsoTimestamp(const QString& text);

} // namespace qbucket::s3::xml
