#pragma once

#include <QDialog>
#include <QUuid>

class QLabel;
class QPushButton;
class QTableView;

namespace qbucket {

class MultipartUploadModel;
class OperationController;
class TransferService;

// "Incomplete Uploads" of a bucket: multipart uploads that were started but
// never completed or aborted — their parts take space (and cost) until a
// lifecycle rule or an abort removes them. Uploads this computer can
// continue are marked. Aborting goes through the OperationController.
class IncompleteUploadsDialog : public QDialog {
    Q_OBJECT

public:
    IncompleteUploadsDialog(OperationController* operations, TransferService* transfers, const QUuid& account,
                            const QString& bucket, QWidget* parent = nullptr);

    [[nodiscard]] MultipartUploadModel* model() const { return m_model; }
    [[nodiscard]] QTableView* view() const { return m_view; }
    [[nodiscard]] QPushButton* abortButton() const { return m_abort; }

private:
    void updateState_();
    void abortSelected_();

    OperationController* m_ops;
    QUuid m_account;
    QString m_bucket;
    MultipartUploadModel* m_model;
    QTableView* m_view = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_abort = nullptr;
};

} // namespace qbucket
