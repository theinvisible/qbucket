#include "controllers/PlanFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QUrl>

namespace qbucket {

PlanFile::PlanFile(const QString& path, QObject* parent)
    : QObject(parent),
      m_path(path)
{
    m_io.setMaxThreadCount(1);
    m_io.setObjectName(QStringLiteral("PlanFile"));
    const QString file = m_path;
    m_io.start([file] {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile::remove(file);   // a leftover of an earlier run is not this plan
    });
}

PlanFile::PlanFile(const QString& path, Mode mode, qint64 records, QObject* parent)
    : QObject(parent),
      m_path(path)
{
    m_io.setMaxThreadCount(1);
    m_io.setObjectName(QStringLiteral("PlanFile"));
    if (mode == Mode::Existing) {
        m_count = records;
        return;
    }
    const QString file = m_path;
    m_io.start([file] {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile::remove(file);
    });
}

QByteArray PlanFile::encodeRecord(const QStringList& fields)
{
    QByteArray line;
    for (qsizetype i = 0; i < fields.size(); ++i) {
        if (i > 0)
            line += ' ';
        // Never empty on disk: an empty field would merge with its neighbour.
        line += fields[i].isEmpty() ? QByteArray("%") : QUrl::toPercentEncoding(fields[i]);
    }
    line += '\n';
    return line;
}

QStringList PlanFile::decodeRecord(const QByteArray& line)
{
    QStringList fields;
    for (const QByteArray& field : line.trimmed().split(' '))
        fields.append(field == "%" ? QString() : QUrl::fromPercentEncoding(field));
    return fields;
}

PlanFile::~PlanFile()
{
    m_io.waitForDone();
    QFile::remove(m_path);
}

void PlanFile::append(const QStringList& keys)
{
    QList<QStringList> records;
    records.reserve(keys.size());
    for (const QString& k : keys)
        records.append({k});
    appendRecords(records);
}

void PlanFile::appendRecords(const QList<QStringList>& records)
{
    if (records.isEmpty())
        return;
    m_count += records.size();
    QByteArray lines;
    for (const QStringList& fields : records)
        lines += encodeRecord(fields);
    const QString file = m_path;
    const QPointer<PlanFile> self(this);
    m_io.start([this, self, file, lines] {
        QFile f(file);
        const bool ok = f.open(QIODevice::Append) && f.write(lines) == lines.size();
        if (ok)
            return;
        const QString error = f.errorString();
        QMetaObject::invokeMethod(this, [self, error] {
            if (self && self->m_error.isEmpty())
                self->m_error = error;
        }, Qt::QueuedConnection);
    });
}

void PlanFile::flush(std::function<void(bool)> done)
{
    const QPointer<PlanFile> self(this);
    m_io.start([this, self, done = std::move(done)] {
        QMetaObject::invokeMethod(this, [self, done] {
            if (self)
                done(self->m_error.isEmpty());
        }, Qt::QueuedConnection);
    });
}

void PlanFile::readNext(int n, std::function<void(const QStringList&)> done)
{
    readNextRecords(n, [done = std::move(done)](const QList<QStringList>& records) {
        QStringList keys;
        keys.reserve(records.size());
        for (const QStringList& r : records)
            keys.append(r.value(0));
        done(keys);
    });
}

void PlanFile::readNextRecords(int n, std::function<void(const QList<QStringList>&)> done)
{
    const QString file = m_path;
    const QPointer<PlanFile> self(this);
    // m_ioOffset lives on the I/O thread: reads queued back to back still
    // follow each other.
    m_io.start([this, self, file, n, done = std::move(done)] {
        QList<QStringList> records;
        QString error;
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(m_ioOffset);
            while (records.size() < n) {
                const QByteArray line = f.readLine();
                if (line.isEmpty())
                    break;
                m_ioOffset += line.size();
                records.append(decodeRecord(line));
            }
        } else if (QFile::exists(file)) {
            error = f.errorString();
        }
        QMetaObject::invokeMethod(this, [self, done, records, error] {
            if (!self)
                return;
            if (!error.isEmpty() && self->m_error.isEmpty())
                self->m_error = error;
            done(records);
        }, Qt::QueuedConnection);
    });
}

void PlanFile::rewind()
{
    m_io.start([this] { m_ioOffset = 0; });
}

} // namespace qbucket
