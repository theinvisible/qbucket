#pragma once

#include <qbucket/s3/Types.h>

#include <QAbstractTableModel>
#include <QSet>
#include <QUuid>

#include <memory>

namespace qbucket {

class AccountSessions;
class TransferService;

// The unfinished multipart uploads of one bucket (ListMultipartUploads, all
// pages up to kMaxUploads), marked when this computer has the records to
// continue one.
class MultipartUploadModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { KeyColumn, StartedColumn, ClassColumn, ResumableColumn, ColumnCount };
    enum Role { UploadIdRole = Qt::UserRole + 1, ResumableRole };
    enum class State { Loading, Ready, Error };
    static constexpr int kMaxUploads = 10000;

    MultipartUploadModel(AccountSessions* sessions, TransferService* transfers, const QUuid& account,
                         const QString& bucket, QObject* parent = nullptr);
    ~MultipartUploadModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] QString errorText() const { return m_error; }
    [[nodiscard]] bool truncated() const { return m_truncated; }
    [[nodiscard]] const s3::MultipartUploadInfo& upload(int row) const { return m_rows[row]; }

    void reload();

signals:
    void stateChanged();

private:
    void fetch_(const QString& keyMarker, const QString& uploadIdMarker, quint64 generation);
    void loadResumable_();

    AccountSessions* m_sessions;
    TransferService* m_transfers;
    QUuid m_account;
    QString m_bucket;
    QList<s3::MultipartUploadInfo> m_rows;
    QList<s3::MultipartUploadInfo> m_incoming;
    QSet<QString> m_resumable;   // upload ids with local records
    State m_state = State::Loading;
    QString m_error;
    bool m_truncated = false;
    quint64 m_generation = 0;
    std::unique_ptr<QObject> m_run;
};

} // namespace qbucket
