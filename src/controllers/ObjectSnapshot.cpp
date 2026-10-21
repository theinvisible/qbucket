#include "controllers/ObjectSnapshot.h"

#include <qbucket/s3/Session.h>

#include <memory>

namespace qbucket {

void ObjectSnapshot::read(s3::Session* session, const QString& bucket, const QString& key, bool withTags,
                          bool withAcl, QObject* context, std::function<void(s3::Result<ObjectSnapshot>)> done)
{
    auto snap = std::make_shared<ObjectSnapshot>();
    auto finish = std::make_shared<std::function<void(s3::Result<ObjectSnapshot>)>>(std::move(done));
    // The ACL last, then done.
    auto readAcl = [session, bucket, key, withAcl, context, snap, finish] {
        if (!withAcl) {
            (*finish)(*snap);
            return;
        }
        session->getObjectAcl(bucket, key, QString(), context, [snap, finish](s3::Result<s3::AccessControlPolicy> acl) {
            if (!acl && acl.error().category == s3::ErrorCategory::Unsupported) {
                snap->aclUnsupported = true;
                (*finish)(*snap);
                return;
            }
            if (!acl) {
                (*finish)(acl.error());
                return;
            }
            snap->acl = *acl;
            (*finish)(*snap);
        });
    };
    session->headObject(bucket, key, QString(), context,
                        [session, bucket, key, withTags, context, snap, finish, readAcl](s3::Result<s3::ObjectMeta> head) {
        if (!head) {
            (*finish)(head.error());
            return;
        }
        snap->meta = *head;
        if (!withTags) {
            readAcl();
            return;
        }
        session->getObjectTagging(bucket, key, QString(), context, [snap, finish, readAcl](s3::Result<s3::ObjectTagging> tags) {
            if (!tags && tags.error().category == s3::ErrorCategory::Unsupported) {
                snap->tagsUnsupported = true;
                readAcl();
                return;
            }
            if (!tags) {
                (*finish)(tags.error());
                return;
            }
            snap->tags = tags->tags;
            readAcl();
        });
    });
}

} // namespace qbucket
