#include "controllers/BrowseController.h"

namespace qbucket {

BrowseController::BrowseController(QObject* parent)
    : QObject(parent)
{
}

void BrowseController::navigate(const Location& to)
{
    if (to == m_current)
        return;
    if (m_current.isValid()) {
        m_back.append(m_current);
        if (m_back.size() > kMaxHistory)
            m_back.removeFirst();
    }
    m_forward.clear();
    moveTo_(to);
}

void BrowseController::back()
{
    if (m_back.isEmpty())
        return;
    if (m_current.isValid())
        m_forward.append(m_current);
    moveTo_(m_back.takeLast());
}

void BrowseController::forward()
{
    if (m_forward.isEmpty())
        return;
    if (m_current.isValid())
        m_back.append(m_current);
    moveTo_(m_forward.takeLast());
}

void BrowseController::up()
{
    if (canGoUp())
        navigate(m_current.parent());
}

void BrowseController::forgetAccount(const QUuid& account)
{
    const auto gone = [&](const Location& l) { return l.account == account; };
    m_back.removeIf(gone);
    m_forward.removeIf(gone);
    // Neighbours that became equal after the removal collapse into one.
    const auto dedupe = [](QList<Location>& list) {
        for (qsizetype i = list.size() - 1; i > 0; --i) {
            if (list[i] == list[i - 1])
                list.removeAt(i);
        }
    };
    dedupe(m_back);
    dedupe(m_forward);
    if (gone(m_current))
        moveTo_(Location{});
    else
        emit historyChanged();
}

void BrowseController::moveTo_(const Location& to)
{
    m_current = to;
    emit locationChanged(m_current);
    emit historyChanged();
}

} // namespace qbucket
