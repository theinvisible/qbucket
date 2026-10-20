#pragma once

#include <qbucket/s3/Types.h>

#include <QModelIndexList>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <memory>

class QFormLayout;
class QLabel;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;

namespace qbucket {

class AccountSessions;

// The right column (PLAN.md §4): Properties of the selected object
// (HeadObject, asked for lazily once the selection rests and dropped when
// it moves on), its headers and user metadata read-only, and a summary for
// a folder or several rows. Tags, Versions and Access arrive with later
// milestones.
class DetailPane : public QWidget {
    Q_OBJECT

public:
    // How long the selection has to rest before an object is asked for.
    static constexpr int kHeadDelayMs = 150;

    explicit DetailPane(AccountSessions* sessions, QWidget* parent = nullptr);

    // Rows of an ObjectTableModel (any column; one per row counts).
    void setSelection(const QUuid& account, const QString& bucket, const QModelIndexList& rows);
    void clear();

    // The object last shown (tests).
    [[nodiscard]] const s3::ObjectMeta& meta() const { return m_meta; }

signals:
    void headFinished();

private:
    void requestHead_();
    void showMeta_(const s3::ObjectMeta& meta);
    void showHint_(const QString& text);
    void addField_(const QString& label, const QString& value, bool selectable = true);
    void resetFields_();

    AccountSessions* m_sessions;
    QUuid m_account;
    QString m_bucket;
    QString m_key;
    s3::ObjectMeta m_meta;
    QTimer m_headDelay;
    std::unique_ptr<QObject> m_run;   // context of the pending HeadObject

    QTabWidget* m_tabs = nullptr;
    QStackedWidget* m_propsStack = nullptr;
    QLabel* m_propsHint = nullptr;
    QWidget* m_props = nullptr;
    QFormLayout* m_form = nullptr;
    QStackedWidget* m_metaStack = nullptr;
    QLabel* m_metaHint = nullptr;
    QTreeWidget* m_headers = nullptr;
};

} // namespace qbucket
