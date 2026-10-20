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

PlanFile::~PlanFile()
{
    m_io.waitForDone();
    QFile::remove(m_path);
}

void PlanFile::append(const QStringList& keys)
{
    if (keys.isEmpty())
        return;
    m_count += keys.size();
    QByteArray lines;
    for (const QString& k : keys)
        lines += QUrl::toPercentEncoding(k) + '\n';
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
    const QString file = m_path;
    const QPointer<PlanFile> self(this);
    // m_ioOffset lives on the I/O thread: reads queued back to back still
    // follow each other.
    m_io.start([this, self, file, n, done = std::move(done)] {
        QStringList keys;
        QString error;
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(m_ioOffset);
            while (keys.size() < n) {
                const QByteArray line = f.readLine();
                if (line.isEmpty())
                    break;
                m_ioOffset += line.size();
                keys.append(QUrl::fromPercentEncoding(line.trimmed()));
            }
        } else if (QFile::exists(file)) {
            error = f.errorString();
        }
        QMetaObject::invokeMethod(this, [self, done, keys, error] {
            if (!self)
                return;
            if (!error.isEmpty() && self->m_error.isEmpty())
                self->m_error = error;
            done(keys);
        }, Qt::QueuedConnection);
    });
}

void PlanFile::rewind()
{
    m_io.start([this] { m_ioOffset = 0; });
}

} // namespace qbucket
