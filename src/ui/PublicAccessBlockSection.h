#pragma once

#include "ui/BucketConfigPage.h"

class QCheckBox;

namespace qbucket {

// The public access block: four switches that keep ACLs and policies from
// making the bucket public.
class PublicAccessBlockSection : public BucketConfigPage {
    Q_OBJECT

public:
    PublicAccessBlockSection(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] QCheckBox* blockAcls() const { return m_blockAcls; }
    [[nodiscard]] QCheckBox* ignoreAcls() const { return m_ignoreAcls; }
    [[nodiscard]] QCheckBox* blockPolicy() const { return m_blockPolicy; }
    [[nodiscard]] QCheckBox* restrictBuckets() const { return m_restrict; }

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override;
    void setEditable_(bool editable) override;
    [[nodiscard]] bool differs_() const override;

private:
    [[nodiscard]] s3::PublicAccessBlockConfiguration edited() const;

    QCheckBox* m_blockAcls = nullptr;
    QCheckBox* m_ignoreAcls = nullptr;
    QCheckBox* m_blockPolicy = nullptr;
    QCheckBox* m_restrict = nullptr;
    s3::PublicAccessBlockConfiguration m_loaded;
};

} // namespace qbucket
