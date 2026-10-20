#pragma once

#include <QStyledItemDelegate>

namespace qbucket {

// Rows of ObjectTableModel: folder / file icon tinted by file type in the
// name column, secondary text for size and date, the storage class as a
// badge. Paints from model roles only — no I/O, nothing per-row cached.
class ObjectRowDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};

} // namespace qbucket
