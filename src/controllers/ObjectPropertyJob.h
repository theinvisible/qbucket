#pragma once

#include "controllers/Job.h"

#include <qbucket/s3/Types.h>

#include <memory>

namespace qbucket {

// Sets one object's tags (the whole set; empty removes them all) or a canned
// ACL — subresource writes (PutObjectTagging / PutObjectAcl), never trusted
// on a read probe's word (Feature::ObjectTaggingWrite / ObjectAclWrite):
//   Plan    — HEAD (size, ETag) and the current tags / ACL for the preview.
//   Execute — HEAD again right before: a changed object is left alone; then
//             the write.
//   Verify  — the object's data is unchanged (size and ETag — a server that
//             took the request for an upload would have replaced it) and the
//             new value reads back.
class ObjectPropertyJob : public Job {
    Q_OBJECT

public:
    enum class Property { Tags, Acl };

    static ObjectPropertyJob* tags(const Account& account, const QString& bucket, const QString& key,
                                   const s3::TagSet& tags, QObject* parent = nullptr);
    static ObjectPropertyJob* acl(const Account& account, const QString& bucket, const QString& key,
                                  const QString& cannedAcl, QObject* parent = nullptr);

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] Property property() const { return m_property; }
    [[nodiscard]] QString key() const { return m_key; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    ObjectPropertyJob(const Account& account, const QString& bucket, const QString& key, Property property,
                      QObject* parent);
    void planned_(const s3::ObjectMeta& meta, const QString& current);
    void write_();
    [[nodiscard]] static QString describeTags(const s3::TagSet& tags);
    [[nodiscard]] Report report_(bool ok, const QString& failure, const QString& verification = {},
                                 bool verified = false) const;

    QString m_key;
    Property m_property;
    s3::TagSet m_tags;
    QString m_canned;
    s3::ObjectMeta m_planned;
    std::unique_ptr<QObject> m_run;
    bool m_written = false;
};

} // namespace qbucket
