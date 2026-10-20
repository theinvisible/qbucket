#include "ui/ObjectRowDelegate.h"

#include "models/ObjectTableModel.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace qbucket {

namespace {

QString extensionOf(const QString& name)
{
    const qsizetype slash = name.lastIndexOf(u'/');
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot > slash + 1 ? name.mid(dot + 1) : QString();
}

} // namespace

void ObjectRowDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    // Rows are selected as a whole: no focus frame around a single cell.
    option->state &= ~QStyle::State_HasFocus;
    const Palette& p = Theme::current();
    switch (index.column()) {
    case ObjectTableModel::NameColumn: {
        const QString name = index.data(ObjectTableModel::NameRole).toString();
        // A key ending in "/" (a folder marker in the flat view) reads as a folder too.
        const bool folder = index.data(ObjectTableModel::IsFolderRole).toBool() || name.endsWith(u'/');
        option->features |= QStyleOptionViewItem::HasDecoration;
        option->decorationSize = QSize(16, 16);
        option->icon = folder ? IconFactory::icon(QStringLiteral("folder"), p.accent, 16)
                              : IconFactory::icon(QStringLiteral("file"), Theme::fileTypeColor(extensionOf(name)), 16);
        break;
    }
    case ObjectTableModel::SizeColumn:
    case ObjectTableModel::ModifiedColumn:
        option->palette.setColor(QPalette::Text, p.text2);
        break;
    default:
        break;
    }
}

void ObjectRowDelegate::paintDropTarget_(QPainter* painter, const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const
{
    if (index.row() != m_dropRow)
        return;
    QColor fill = Theme::current().accent;
    fill.setAlpha(60);
    painter->save();
    painter->fillRect(option.rect, fill);
    painter->setPen(Theme::current().accent);
    painter->drawLine(option.rect.topLeft(), option.rect.topRight());
    painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    painter->restore();
}

void ObjectRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    if (index.column() != ObjectTableModel::StorageClassColumn
        || index.data(ObjectTableModel::IsFolderRole).toBool()) {
        QStyledItemDelegate::paint(painter, option, index);
        paintDropTarget_(painter, option, index);
        return;
    }

    // Background, focus and selection from the style; the badge on top.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const Theme::StorageClassBadge badge =
        Theme::storageClassBadge(index.data(ObjectTableModel::StorageClassRole).toString());
    opt.text.clear();
    const QWidget* widget = opt.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    painter->save();
    if (!badge.tinted) {
        painter->setPen(Theme::current().text2);
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                          opt.fontMetrics.elidedText(badge.label, Qt::ElideRight, text.width()));
        painter->restore();
        return;
    }
    QFont font = opt.font;
    font.setPointSizeF(qMax(6.0, font.pointSizeF() - 1.0));
    font.setWeight(QFont::DemiBold);
    const QFontMetrics fm(font);
    const int w = qMin(text.width(), fm.horizontalAdvance(badge.label) + 12);
    const int h = fm.height() + 2;
    const QRect pill(text.left(), text.center().y() - h / 2, w, h);
    painter->setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(pill), h / 2.0, h / 2.0);
    QColor fill = badge.fg;
    fill.setAlpha(Theme::isDark() ? 56 : 36);
    painter->fillPath(path, fill);
    painter->setPen(badge.fg);
    painter->setFont(font);
    painter->drawText(pill, Qt::AlignCenter, fm.elidedText(badge.label, Qt::ElideRight, pill.width() - 8));
    painter->restore();
}

} // namespace qbucket
