#pragma once

#include "domain/Location.h"

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QStackedWidget;

namespace qbucket {

// Breadcrumb above the object list (PLAN.md §4): environment badge,
// account › bucket › folder › …, each crumb clickable. Long paths fold
// their leading folders into a "…" menu. editPath() swaps in a line edit
// for typing "bucket/prefix/" (Enter goes there, Escape cancels).
class PathBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int kVisibleSegments = 6;

    explicit PathBar(QWidget* parent = nullptr);

    void setLocation(const QString& accountName, int environment, const Location& location);
    // No location: a hint instead of crumbs.
    void clearLocation(const QString& hint);
    void editPath();

    [[nodiscard]] Location location() const { return m_location; }

signals:
    // A crumb was clicked: the bucket root or one of the folders on the way.
    void crumbActivated(const QString& bucket, const QString& prefix);
    // Typed in the edit mode; prefix ends with "/" or is empty.
    void pathEntered(const QString& bucket, const QString& prefix);

private:
    void rebuild_();
    void finishEdit_(bool accept);
    void addCrumb_(const QString& text, const QString& prefix, bool current);
    void addSeparator_();

    Location m_location;
    QString m_accountName;
    int m_environment = 0;
    QString m_hint;

    QStackedWidget* m_stack = nullptr;
    QWidget* m_crumbs = nullptr;
    QHBoxLayout* m_crumbLayout = nullptr;
    QLineEdit* m_edit = nullptr;
};

} // namespace qbucket
