#pragma once

#include <qbucket/s3/Result.h>
#include <qbucket/s3/Types.h>

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace qbucket {

namespace s3 {
class Session;
}

// The whole current state of one object an edit has to carry over (PLAN.md
// §12.3: metadata edits merge the full current state): HEAD (headers, user
// metadata, class, encryption, lock), and its tags and ACL where the server
// has them.
struct ObjectSnapshot {
    s3::ObjectMeta meta;
    std::optional<s3::TagSet> tags;                 // nullopt: not read
    std::optional<s3::AccessControlPolicy> acl;     // nullopt: not read
    // The server answered "not implemented": the object has none to lose.
    bool tagsUnsupported = false;
    bool aclUnsupported = false;

    // HEAD, then the tags and the ACL as asked; any failure but "not
    // implemented" for tags / ACL fails the read.
    // `done` runs on the context's thread.
    static void read(s3::Session* session, const QString& bucket, const QString& key, bool withTags, bool withAcl,
                     QObject* context, std::function<void(s3::Result<ObjectSnapshot>)> done);
};

} // namespace qbucket
