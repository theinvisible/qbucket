#include "controllers/ActivityLog.h"

namespace qbucket {

void ActivityLog::add(Level level, const QString& text)
{
    Entry e{QDateTime::currentDateTimeUtc(), level, text};
    m_entries.append(e);
    if (m_entries.size() > kKeep)
        m_entries.remove(0, m_entries.size() - kKeep);
    emit added(e);
}

} // namespace qbucket
