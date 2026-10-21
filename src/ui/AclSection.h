#pragma once

#include "ui/BucketConfigPage.h"

class QComboBox;
class QTreeWidget;

namespace qbucket {

// The bucket ACL: a canned ACL to set, the grants as they are (read-only —
// grants a canned ACL can't express are replaced by one, the preview says
// so).
class AclSection : public BucketConfigPage {
    Q_OBJECT

public:
    AclSection(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] QComboBox* cannedBox() const { return m_canned; }
    [[nodiscard]] QTreeWidget* grants() const { return m_grants; }

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override { return {}; }
    [[nodiscard]] QString editedCannedAcl_() const override;
    void setEditable_(bool editable) override;
    [[nodiscard]] QString problem_() const override;
    [[nodiscard]] bool differs_() const override;
    [[nodiscard]] QString stateText_(const s3::BucketConfigDocument& doc) const override;

private:
    QComboBox* m_canned = nullptr;
    QTreeWidget* m_grants = nullptr;
    QString m_loadedCanned;   // empty: grants a canned ACL can't express
};

} // namespace qbucket
