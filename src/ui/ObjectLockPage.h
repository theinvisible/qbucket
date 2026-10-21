#pragma once

#include "ui/BucketConfigPage.h"

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace qbucket {

// Object lock: whether the bucket has it (only possible when it is
// created) and the default retention new versions get — GOVERNANCE or
// COMPLIANCE, for days or years. Off: versions get none unless an upload
// asks for one.
class ObjectLockPage : public BucketConfigPage {
    Q_OBJECT

public:
    ObjectLockPage(const Context& context, QWidget* parent = nullptr);

    [[nodiscard]] QCheckBox* defaultCheck() const { return m_default; }
    [[nodiscard]] QComboBox* modeBox() const { return m_mode; }
    [[nodiscard]] QSpinBox* periodBox() const { return m_period; }
    [[nodiscard]] QComboBox* unitBox() const { return m_unit; }

protected:
    void showDocument_(const s3::BucketConfigDocument& doc) override;
    [[nodiscard]] QByteArray editedDocument_() const override;
    void setEditable_(bool editable) override;
    [[nodiscard]] bool differs_() const override;
    [[nodiscard]] QString problem_() const override;
    [[nodiscard]] QString stateText_(const s3::BucketConfigDocument& doc) const override;

private:
    [[nodiscard]] s3::ObjectLockConfiguration edited() const;
    void updateEnabled_();

    QCheckBox* m_default = nullptr;
    QComboBox* m_mode = nullptr;
    QSpinBox* m_period = nullptr;
    QComboBox* m_unit = nullptr;
    s3::ObjectLockConfiguration m_loaded;
    bool m_editable = false;
};

} // namespace qbucket
