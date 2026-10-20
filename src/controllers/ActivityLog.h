#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

namespace qbucket {

// What the app did and noticed, for the Log tab: capability changes now,
// the operation journal's summaries later. In memory, the newest kKeep
// entries. Never holds secrets — callers pass texts meant for the user.
//
// Thread affinity: GUI thread.
class ActivityLog : public QObject {
    Q_OBJECT

public:
    enum class Level { Info, Warning, Error };
    Q_ENUM(Level)
    struct Entry {
        QDateTime at;   // UTC
        Level level = Level::Info;
        QString text;
    };
    static constexpr int kKeep = 2000;

    using QObject::QObject;

    void add(Level level, const QString& text);
    [[nodiscard]] const QList<Entry>& entries() const { return m_entries; }

signals:
    void added(const qbucket::ActivityLog::Entry& entry);

private:
    QList<Entry> m_entries;
};

} // namespace qbucket
