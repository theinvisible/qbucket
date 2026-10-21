#pragma once

#include "controllers/AccountImporter.h"

#include <QDialog>
#include <QList>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace qbucket {

class AccountStore;
class ConnectionTester;
class SecretStore;

// "Import Accounts": the profiles of the AWS CLI's files or the S3 remotes
// of rclone's configuration, each with a suggested provider profile the
// user can change, the ones already set up and the ones that can't be
// imported (with why) unticked. The default files are read when visible;
// "Choose File…" picks others (through the file chooser portal in a
// sandbox). Imported accounts get one environment and read-only setting.
// Open it with open(), never exec().
class ImportAccountsDialog : public QDialog {
    Q_OBJECT

public:
    enum Column { ImportColumn, NameColumn, ProviderColumn, RegionColumn, EndpointColumn, StatusColumn };

    ImportAccountsDialog(AccountStore* accounts, SecretStore* secrets, QWidget* parent = nullptr);
    ~ImportAccountsDialog() override;

    [[nodiscard]] AccountImporter* importer() const { return m_importer; }
    [[nodiscard]] QTableWidget* table() const { return m_table; }
    [[nodiscard]] QPushButton* importButton() const { return m_import; }
    [[nodiscard]] QPushButton* testButton() const { return m_test; }
    [[nodiscard]] QLabel* statusLabel() const { return m_status; }
    // Switches the source and reads `files` (empty: the defaults).
    void load(AccountImporter::Source source, const QStringList& files = {});

signals:
    void imported(int count, bool sessionOnly);

private:
    void showCandidates_(const QList<ImportCandidate>& candidates, const QString& problem);
    void chooseFiles_();
    [[nodiscard]] QList<ImportCandidate> chosen_() const;
    [[nodiscard]] ImportCandidate candidateAt_(int row) const;
    void updateButtons_();
    void testNext_();
    void import_();

    AccountStore* m_accounts;
    SecretStore* m_secrets;
    AccountImporter* m_importer;
    ConnectionTester* m_tester;
    AccountImporter::Source m_source = AccountImporter::Source::Aws;
    QList<ImportCandidate> m_candidates;
    QButtonGroup* m_sourceGroup = nullptr;
    QLabel* m_files = nullptr;
    QTableWidget* m_table = nullptr;
    QComboBox* m_environment = nullptr;
    QCheckBox* m_readOnly = nullptr;
    bool m_readOnlyTouched = false;
    QLabel* m_hint = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_test = nullptr;
    QPushButton* m_import = nullptr;
    QList<int> m_testQueue;
    int m_testing = -1;
};

} // namespace qbucket
