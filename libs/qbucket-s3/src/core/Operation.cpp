#include <qbucket/s3/Operation.h>

namespace qbucket::s3 {

QString operationName(Operation op)
{
    switch (op) {
    case Operation::ListBuckets:          return QStringLiteral("ListBuckets");
    case Operation::ListObjectsV2:        return QStringLiteral("ListObjectsV2");
    case Operation::ListObjectsV1:        return QStringLiteral("ListObjects");
    case Operation::HeadBucket:           return QStringLiteral("HeadBucket");
    case Operation::GetBucketLocation:    return QStringLiteral("GetBucketLocation");
    case Operation::HeadObject:           return QStringLiteral("HeadObject");
    case Operation::GetObject:            return QStringLiteral("GetObject");
    case Operation::PutObject:            return QStringLiteral("PutObject");
    case Operation::DeleteObject:         return QStringLiteral("DeleteObject");
    case Operation::DeleteObjects:        return QStringLiteral("DeleteObjects");
    case Operation::CreateBucket:         return QStringLiteral("CreateBucket");
    case Operation::DeleteBucket:         return QStringLiteral("DeleteBucket");
    case Operation::GetBucketSubresource: return QStringLiteral("GetBucketSubresource");
    case Operation::GetObjectSubresource: return QStringLiteral("GetObjectSubresource");
    case Operation::CopyObject:           return QStringLiteral("CopyObject");
    case Operation::CreateMultipartUpload:   return QStringLiteral("CreateMultipartUpload");
    case Operation::UploadPart:              return QStringLiteral("UploadPart");
    case Operation::CompleteMultipartUpload: return QStringLiteral("CompleteMultipartUpload");
    case Operation::AbortMultipartUpload:    return QStringLiteral("AbortMultipartUpload");
    case Operation::ListParts:               return QStringLiteral("ListParts");
    case Operation::ListMultipartUploads:    return QStringLiteral("ListMultipartUploads");
    case Operation::UploadPartCopy:          return QStringLiteral("UploadPartCopy");
    }
    return QStringLiteral("Unknown");
}

bool isMutating(Operation op)
{
    switch (op) {
    case Operation::PutObject:
    case Operation::DeleteObject:
    case Operation::DeleteObjects:
    case Operation::CreateBucket:
    case Operation::DeleteBucket:
    case Operation::CopyObject:
    case Operation::CreateMultipartUpload:
    case Operation::UploadPart:
    case Operation::CompleteMultipartUpload:
    case Operation::AbortMultipartUpload:
    case Operation::UploadPartCopy:
        return true;
    default:
        return false;
    }
}

} // namespace qbucket::s3
