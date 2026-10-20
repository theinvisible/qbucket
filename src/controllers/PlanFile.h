#pragma once

#include <QObject>
#include <QStringList>
#include <QThreadPool>

#include <functional>

namespace qbucket {

// The keys a bulk job will touch, on disk instead of in memory (PLAN.md
// §13.2: list page → plan file → act in batches): one percent-encoded key
// per line (keys may contain line breaks). Written while planning, read
// back in batches while executing. File I/O runs on its own thread, in
// call order; callbacks come back to this object's thread and are dropped
// with it. The file is removed when the PlanFile is destroyed.
//
// Thread affinity: GUI thread.
class PlanFile : public QObject {
    Q_OBJECT

public:
    explicit PlanFile(const QString& path, QObject* parent = nullptr);
    ~PlanFile() override;

    [[nodiscard]] QString path() const { return m_path; }
    [[nodiscard]] qint64 count() const { return m_count; }
    [[nodiscard]] bool failed() const { return !m_error.isEmpty(); }
    [[nodiscard]] QString errorText() const { return m_error; }

    void append(const QStringList& keys);
    // Called once everything appended before has been written (or failed).
    void flush(std::function<void(bool ok)> done);
    // The next up to `n` keys after those read (or asked for) so far; empty
    // at the end. Calls are served in order.
    void readNext(int n, std::function<void(const QStringList& keys)> done);
    void rewind();

private:
    QString m_path;
    qint64 m_count = 0;
    qint64 m_ioOffset = 0;   // bytes read so far — touched on the I/O thread only
    QString m_error;
    QThreadPool m_io;
};

} // namespace qbucket
