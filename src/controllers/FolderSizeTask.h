#pragma once

#include <qbucket/s3/ListObjectsPager.h>

#include <QMap>
#include <QObject>
#include <QStringList>

#include <memory>

namespace qbucket {

namespace s3 {
class Session;
}

// Counts the objects and bytes below folder prefixes (PLAN.md §6 M4, §13.2):
// a flat listing page by page, nothing kept but the sums — a cancellable
// background task, started on request, never on opening a folder. It
// changes nothing, so it is no Job. Folder markers ("x/") are not counted.
// An error stays an error (never "0 objects").
//
// GUI thread; the listing runs on the session's worker.
class FolderSizeTask : public QObject {
    Q_OBJECT

public:
    struct Totals {
        qint64 objects = 0;
        qint64 bytes = 0;
        QMap<QString, std::pair<qint64, qint64>> byClass;   // class → (objects, bytes); "" → STANDARD
        int pages = 0;
    };

    FolderSizeTask(s3::Session* session, const QString& bucket, const QStringList& prefixes, QObject* parent = nullptr);
    ~FolderSizeTask() override;

    void start();
    void cancel();

    [[nodiscard]] const Totals& totals() const { return m_totals; }
    [[nodiscard]] bool isRunning() const { return m_running; }
    [[nodiscard]] bool isComplete() const { return m_complete; }
    [[nodiscard]] QString errorText() const { return m_error; }

signals:
    void progress();   // after every page
    void finished();   // complete, failed or cancelled

private:
    void nextPrefix_();
    void fetch_();
    void end_();

    s3::Session* m_session;
    QString m_bucket;
    QStringList m_prefixes;
    int m_index = 0;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    std::unique_ptr<QObject> m_run;
    Totals m_totals;
    bool m_running = false;
    bool m_complete = false;
    QString m_error;
};

} // namespace qbucket
