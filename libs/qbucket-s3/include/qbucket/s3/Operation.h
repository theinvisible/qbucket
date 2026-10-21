#pragma once

#include <qbucket/s3/Export.h>

#include <QString>
#include <QtGlobal>

namespace qbucket::s3 {

// Every S3 operation the library can send. Used for logging, the request
// observer, provider hooks and the capability short-circuit.
//
// Append-only: values may be persisted by consumers (journals).
enum class Operation : quint16 {
    ListBuckets,
    ListObjectsV2,
    ListObjectsV1,
    HeadBucket,
    GetBucketLocation,
    HeadObject,
    GetObject,
    PutObject,
    DeleteObject,
    DeleteObjects,
    CreateBucket,
    DeleteBucket,
    GetBucketSubresource,   // capability probes: ?versioning, ?cors, …
    GetObjectSubresource,   // capability probes: ?tagging, ?acl, …
    CopyObject,
    CreateMultipartUpload,
    UploadPart,
    CompleteMultipartUpload,
    AbortMultipartUpload,
    ListParts,
    ListMultipartUploads,
    UploadPartCopy,
    GetObjectTagging,
    PutObjectTagging,       // no DeleteObjectTagging: an empty tag set is written instead
    GetObjectAcl,
    PutObjectAcl,
    ListObjectVersions,
    GetBucketVersioning,
    PutBucketVersioning,
};

// Stable English name, e.g. "ListObjectsV2".
QBUCKET_S3_EXPORT QString operationName(Operation op);

// True for operations that change data or configuration on the server.
QBUCKET_S3_EXPORT bool isMutating(Operation op);

} // namespace qbucket::s3
