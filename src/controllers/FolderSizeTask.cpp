#include "controllers/FolderSizeTask.h"

#include "controllers/ConnectionTester.h"

#include <qbucket/s3/Session.h>

namespace qbucket {

FolderSizeTask::FolderSizeTask(s3::Session* session, const QString& bucket, const QStringList& prefixes, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_bucket(bucket),
      m_prefixes(prefixes)
{
}

FolderSizeTask::~FolderSizeTask() = default;

void FolderSizeTask::start()
{
    if (m_running)
        return;
    m_running = true;
    m_complete = false;
    m_error.clear();
    m_totals = Totals();
    m_index = 0;
    m_run = std::make_unique<QObject>();
    nextPrefix_();
}

void FolderSizeTask::cancel()
{
    if (!m_running)
        return;
    m_run.reset();   // answers still under way are dropped
    m_pager.reset();
    m_error = tr("cancelled");
    end_();
}

void FolderSizeTask::nextPrefix_()
{
    if (m_index >= m_prefixes.size()) {
        m_complete = true;
        end_();
        return;
    }
    s3::ListObjectsParams params;
    params.bucket = m_bucket;
    params.prefix = m_prefixes[m_index++];   // flat: everything below
    params.maxKeys = 1000;
    m_pager = std::make_unique<s3::ListObjectsPager>(*m_session, params);
    fetch_();
}

void FolderSizeTask::fetch_()
{
    m_pager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
        if (!m_running)
            return;
        if (!page) {
            m_error = ConnectionTester::describe(page.error());
            end_();
            return;
        }
        for (const s3::ObjectEntry& e : page->objects) {
            if (e.key.endsWith(u'/'))
                continue;   // a folder marker
            ++m_totals.objects;
            m_totals.bytes += e.size;
            auto& [count, bytes] = m_totals.byClass[e.storageClass.isEmpty() ? QStringLiteral("STANDARD") : e.storageClass];
            ++count;
            bytes += e.size;
        }
        ++m_totals.pages;
        emit progress();
        if (m_pager->hasMore()) {
            fetch_();
            return;
        }
        nextPrefix_();
    });
}

void FolderSizeTask::end_()
{
    m_running = false;
    emit finished();
}

} // namespace qbucket
