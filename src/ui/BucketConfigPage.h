#pragma once

#include <qbucket/s3/BucketConfig.h>

#include <QUuid>
#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace qbucket {

class AccountSessions;
class CapabilityController;
class OperationController;

// One bucket configuration in BucketPropertiesDialog: read fresh the first
// time it is shown, edited in a subclass's editor, applied or removed only
// through SetBucketConfigJob (OperationController). What it shows:
//   - "Not supported by this server" / "No permission" (the read feature),
//   - read-only with the reason where the kind's write isn't confirmed or
//     the account is read-only,
//   - Apply when the edit differs and is valid (the problem is shown live),
//     Revert, and Remove for kinds that can be removed.
// Subclasses build their editor into editorArea() and implement the hooks.
class BucketConfigPage : public QWidget {
    Q_OBJECT

public:
    struct Context {
        QUuid account;
        QString bucket;
        AccountSessions* sessions = nullptr;
        OperationController* operations = nullptr;
        CapabilityController* capabilities = nullptr;
    };

    BucketConfigPage(s3::BucketConfig kind, const Context& context, QWidget* parent = nullptr);
    ~BucketConfigPage() override;

    [[nodiscard]] s3::BucketConfig kind() const { return m_kind; }
    [[nodiscard]] const Context& context() const { return m_context; }
    // Reads it unless done already (also on the first showEvent).
    void ensureLoaded();
    void reload();
    [[nodiscard]] bool isLoaded() const { return m_loaded; }
    [[nodiscard]] bool isRunning() const { return m_running; }
    [[nodiscard]] bool isDirty() const;
    [[nodiscard]] bool isEditable() const { return m_editable; }
    [[nodiscard]] const s3::BucketConfigDocument& loadedDocument() const { return m_document; }
    // Why the page can't change anything (empty: it can).
    [[nodiscard]] QString readOnlyReason() const { return m_readOnlyReason; }

    [[nodiscard]] QPushButton* applyButton() const { return m_apply; }
    [[nodiscard]] QPushButton* revertButton() const { return m_revert; }
    [[nodiscard]] QPushButton* removeButton() const { return m_remove; }
    [[nodiscard]] QLabel* stateLabel() const { return m_state; }
    [[nodiscard]] QLabel* problemLabel() const { return m_problem; }

signals:
    // After every read (opening, Revert, after a change).
    void loaded();
    void dirtyChanged();

protected:
    [[nodiscard]] QVBoxLayout* editorArea() const { return m_editorArea; }
    // Fills the editor from what was read (`doc.configured` false: nothing set).
    virtual void showDocument_(const s3::BucketConfigDocument& doc) = 0;
    // The document the editor holds now.
    [[nodiscard]] virtual QByteArray editedDocument_() const = 0;
    // The ACL: a canned ACL instead of a document.
    [[nodiscard]] virtual QString editedCannedAcl_() const { return {}; }
    virtual void setEditable_(bool editable) = 0;
    // Why the edit can't be applied (empty: it can); default: the job's check.
    [[nodiscard]] virtual QString problem_() const;
    // The state line for what was read; default: "Set" / "Not set".
    [[nodiscard]] virtual QString stateText_(const s3::BucketConfigDocument& doc) const;
    // Whether the edit differs from what was read; default: as documents.
    [[nodiscard]] virtual bool differs_() const;
    // A line under the state, e.g. what the form can't show.
    void setNote_(const QString& note);
    // Subclasses call this on every edit.
    void edited_();
    void showEvent(QShowEvent* event) override;

private:
    void update_();
    void startJob_(bool remove);

    s3::BucketConfig m_kind;
    Context m_context;
    std::unique_ptr<QObject> m_run;
    s3::BucketConfigDocument m_document;
    bool m_loaded = false;
    bool m_loading = false;
    bool m_running = false;
    bool m_editable = false;
    bool m_wasDirty = false;
    bool m_filling = false;
    QString m_readOnlyReason;
    QString m_loadError;

    QLabel* m_state = nullptr;
    QLabel* m_note = nullptr;
    QVBoxLayout* m_editorArea = nullptr;
    QLabel* m_problem = nullptr;
    QLabel* m_reason = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_revert = nullptr;
    QPushButton* m_remove = nullptr;
};

} // namespace qbucket
