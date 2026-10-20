#pragma once

#include <QWidget>

class QStackedWidget;
class QTableView;
class QToolButton;

namespace qbucket {

class TransferModel;

// The "Transfers" tab of the bottom dock: every upload and download of
// this session with a progress bar, speed, time left and state; pause,
// resume, cancel, retry what failed, clear the finished ones.
class TransferPanel : public QWidget {
    Q_OBJECT

public:
    explicit TransferPanel(TransferModel* model, QWidget* parent = nullptr);

    [[nodiscard]] QTableView* view() const { return m_view; }
    [[nodiscard]] QToolButton* pauseButton() const { return m_pause; }
    [[nodiscard]] QToolButton* resumeButton() const { return m_resume; }
    [[nodiscard]] QToolButton* cancelButton() const { return m_cancel; }
    [[nodiscard]] QToolButton* retryButton() const { return m_retry; }
    [[nodiscard]] QToolButton* clearButton() const { return m_clear; }

private:
    void updateButtons_();
    [[nodiscard]] int currentRow_() const;

    TransferModel* m_model;
    QStackedWidget* m_stack = nullptr;
    QTableView* m_view = nullptr;
    QToolButton* m_pause = nullptr;
    QToolButton* m_resume = nullptr;
    QToolButton* m_cancel = nullptr;
    QToolButton* m_retry = nullptr;
    QToolButton* m_clear = nullptr;
};

} // namespace qbucket
