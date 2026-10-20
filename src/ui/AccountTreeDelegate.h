#pragma once

#include <QStyledItemDelegate>

namespace qbucket {

// Sidebar rows of AccountTreeModel: account rows get the environment badge
// (Production red, Staging amber, Development green — PLAN.md §12.1) and a
// lock when read-only; buckets and status rows get their icons. Paints
// from model roles only, no I/O.
class AccountTreeDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    // Badge text and colour of an environment (int(Environment)).
    [[nodiscard]] static QString environmentLabel(int environment);
    [[nodiscard]] static QColor environmentColor(int environment);

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};

} // namespace qbucket
