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
    case Operation::GetObjectTagging:        return QStringLiteral("GetObjectTagging");
    case Operation::PutObjectTagging:        return QStringLiteral("PutObjectTagging");
    case Operation::GetObjectAcl:            return QStringLiteral("GetObjectAcl");
    case Operation::PutObjectAcl:            return QStringLiteral("PutObjectAcl");
    case Operation::ListObjectVersions:      return QStringLiteral("ListObjectVersions");
    case Operation::GetBucketVersioning:     return QStringLiteral("GetBucketVersioning");
    case Operation::PutBucketVersioning:     return QStringLiteral("PutBucketVersioning");
    case Operation::GetBucketPolicy:             return QStringLiteral("GetBucketPolicy");
    case Operation::PutBucketPolicy:             return QStringLiteral("PutBucketPolicy");
    case Operation::DeleteBucketPolicy:          return QStringLiteral("DeleteBucketPolicy");
    case Operation::GetBucketPolicyStatus:       return QStringLiteral("GetBucketPolicyStatus");
    case Operation::GetBucketCors:               return QStringLiteral("GetBucketCors");
    case Operation::PutBucketCors:               return QStringLiteral("PutBucketCors");
    case Operation::DeleteBucketCors:            return QStringLiteral("DeleteBucketCors");
    case Operation::GetBucketLifecycle:          return QStringLiteral("GetBucketLifecycleConfiguration");
    case Operation::PutBucketLifecycle:          return QStringLiteral("PutBucketLifecycleConfiguration");
    case Operation::DeleteBucketLifecycle:       return QStringLiteral("DeleteBucketLifecycle");
    case Operation::GetObjectLockConfiguration:  return QStringLiteral("GetObjectLockConfiguration");
    case Operation::PutObjectLockConfiguration:  return QStringLiteral("PutObjectLockConfiguration");
    case Operation::GetBucketAcl:                return QStringLiteral("GetBucketAcl");
    case Operation::PutBucketAcl:                return QStringLiteral("PutBucketAcl");
    case Operation::GetPublicAccessBlock:        return QStringLiteral("GetPublicAccessBlock");
    case Operation::PutPublicAccessBlock:        return QStringLiteral("PutPublicAccessBlock");
    case Operation::DeletePublicAccessBlock:     return QStringLiteral("DeletePublicAccessBlock");
    case Operation::GetBucketTagging:            return QStringLiteral("GetBucketTagging");
    case Operation::GetBucketWebsite:            return QStringLiteral("GetBucketWebsite");
    case Operation::GetBucketLogging:            return QStringLiteral("GetBucketLogging");
    case Operation::GetBucketReplication:        return QStringLiteral("GetBucketReplication");
    case Operation::GetBucketEncryption:         return QStringLiteral("GetBucketEncryption");
    case Operation::GetBucketNotification:       return QStringLiteral("GetBucketNotificationConfiguration");
    case Operation::GetBucketOwnershipControls:  return QStringLiteral("GetBucketOwnershipControls");
    case Operation::GetObjectRetention:          return QStringLiteral("GetObjectRetention");
    case Operation::PutObjectRetention:          return QStringLiteral("PutObjectRetention");
    case Operation::GetObjectLegalHold:          return QStringLiteral("GetObjectLegalHold");
    case Operation::PutObjectLegalHold:          return QStringLiteral("PutObjectLegalHold");
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
    case Operation::PutObjectTagging:
    case Operation::PutObjectAcl:
    case Operation::PutBucketVersioning:
    case Operation::PutBucketPolicy:
    case Operation::DeleteBucketPolicy:
    case Operation::PutBucketCors:
    case Operation::DeleteBucketCors:
    case Operation::PutBucketLifecycle:
    case Operation::DeleteBucketLifecycle:
    case Operation::PutObjectLockConfiguration:
    case Operation::PutBucketAcl:
    case Operation::PutPublicAccessBlock:
    case Operation::DeletePublicAccessBlock:
    case Operation::PutObjectRetention:
    case Operation::PutObjectLegalHold:
        return true;
    default:
        return false;
    }
}

} // namespace qbucket::s3
