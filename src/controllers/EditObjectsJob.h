#pragma once

#include "controllers/Job.h"
#include "controllers/ObjectSnapshot.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QMap>
#include <QSet>

#include <memory>
#include <optional>

namespace qbucket {

class InPlaceCopy;

// Changes headers, user metadata or the storage class of objects in place
// (PLAN.md §6 M4, §12.3) — a copy of each object onto itself with
// MetadataDirective REPLACE, which keeps only what it sends. So, per object:
//   - a fresh snapshot (HEAD, tags, ACL), the change merged into it — never
//     into what a dialog showed earlier;
//   - skipped, with the reason, when the copy would lose something it can't
//     carry: SSE-C / KMS encryption, retention or a legal hold on the
//     version, an archive class, a custom ACL, tags or a public ACL where
//     writing them isn't confirmed, headers that aren't plain ASCII;
//   - the copy with x-amz-copy-source-if-match (a concurrent change aborts),
//     the canned ACL and SSE-S3 sent along; above 5 GiB a multipart copy;
//   - tags the server dropped (RGW does on such copies) written back;
//   - verified: size, the content's ETag where it is an MD5, every header,
//     the class, tags and ACL — once more after a moment (Hetzner's reads
//     lag), then a mismatch is a failure.
// The scope is listed afresh (folders: every object below; folder markers
// are left alone). The copy and its check are InPlaceCopy's.
class EditObjectsJob : public Job {
    Q_OBJECT

public:
    // What to change; unset fields stay as they are on each object. A set
    // field with an empty value removes that header.
    struct ChangeSpec {
        std::optional<QString> contentType;
        std::optional<QString> cacheControl;
        std::optional<QString> contentDisposition;
        std::optional<QString> contentEncoding;
        std::optional<QString> contentLanguage;
        std::optional<QString> expires;
        std::optional<QString> websiteRedirectLocation;
        QMap<QString, std::optional<QString>> metadata;   // name (lower case) → value; nullopt removes
        std::optional<QString> storageClass;

        [[nodiscard]] bool isEmpty() const;
        [[nodiscard]] bool headersOnly() const { return !storageClass; }
        [[nodiscard]] s3::ObjectHeaders applyTo(const s3::ObjectHeaders& headers) const;
        // For the preview: one line per change.
        [[nodiscard]] QStringList describe() const;
        // What turns `before` into `after` (a single object's dialog).
        [[nodiscard]] static ChangeSpec between(const s3::ObjectHeaders& before, const s3::ObjectHeaders& after);
    };
    struct Item {
        QString key;           // an object key, or a folder prefix ending with "/"
        bool folder = false;
    };
    static constexpr int kParallel = 4;
    static constexpr int kSamples = 10;
    static constexpr qint64 kCopyLimit = Q_INT64_C(5) * 1024 * 1024 * 1024;

    EditObjectsJob(const Account& account, const QString& bucket, const QList<Item>& items, ChangeSpec change,
                   const QString& planDir, QObject* parent = nullptr);
    ~EditObjectsJob() override;

    [[nodiscard]] QString title() const override;
    // Objects above 5 GiB are copied in parts.
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] const ChangeSpec& change() const { return m_change; }
    // For tests: the pause before the second look at a mismatch.
    void setRecheckDelayForTesting(int ms);

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Entry {
        QString key;
        qint64 size = 0;
        QString etag;
    };
    struct Active;
    // plan
    void checkCapabilities_();
    void nextItem_();
    void fetchPage_();
    void planDone_();
    // execute
    void pump_();
    void start_(const Entry& e);
    void snapshotTaken_(const std::shared_ptr<Active>& a);
    void done_(const std::shared_ptr<Active>& a, const QString& failure);
    void skipped_(const QString& key, const QString& why);
    // Why this object can't be edited safely; empty: it can.
    [[nodiscard]] QString cannotEdit_(const ObjectSnapshot& s) const;
    void settled_();
    [[nodiscard]] Report report_() const;

    QList<Item> m_items;
    ChangeSpec m_change;
    QString m_planDir;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    InPlaceCopy* m_copy = nullptr;   // child
    int m_itemIndex = 0;
    bool m_large = false;
    qint64 m_bytes = 0;
    QStringList m_samples;
    // what the server can tell / take (decided in the plan)
    bool m_readTags = false;
    bool m_readAcl = false;
    bool m_writeTags = false;
    bool m_writeAcl = false;

    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    qint64 m_skipped = 0;
    QStringList m_notes;
    QStringList m_failedKeys;
    QStringList m_failures;
};

} // namespace qbucket
