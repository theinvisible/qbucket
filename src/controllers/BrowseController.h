#pragma once

#include "domain/Location.h"

#include <QList>
#include <QObject>

namespace qbucket {

// Where the object browser is and how it got there: back / forward / up
// like a file manager, for this session only. Knows nothing about
// listings; the view follows locationChanged.
//
// Thread affinity: GUI thread.
class BrowseController : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxHistory = 100;

    explicit BrowseController(QObject* parent = nullptr);

    [[nodiscard]] Location current() const { return m_current; }
    [[nodiscard]] bool canGoBack() const { return !m_back.isEmpty(); }
    [[nodiscard]] bool canGoForward() const { return !m_forward.isEmpty(); }
    [[nodiscard]] bool canGoUp() const { return m_current.hasBucket() && !m_current.prefix.isEmpty(); }

    // A new place: the current one goes onto the back stack, forward is
    // cleared. Navigating to where we are does nothing.
    void navigate(const Location& to);
    void back();
    void forward();
    void up();
    // Drops an account from the history (it was removed); if it is the
    // current one, the browser shows no location.
    void forgetAccount(const QUuid& account);

signals:
    void locationChanged(const qbucket::Location& location);
    void historyChanged();

private:
    void moveTo_(const Location& to);

    Location m_current;
    QList<Location> m_back;      // most recent last
    QList<Location> m_forward;   // most recent last
};

} // namespace qbucket
