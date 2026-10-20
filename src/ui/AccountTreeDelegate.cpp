#include "ui/AccountTreeDelegate.h"

#include "domain/Account.h"
#include "models/AccountTreeModel.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace qbucket {

namespace {

constexpr int kBadgePadX = 5;
constexpr int kBadgeGap = 6;
constexpr int kLockSize = 14;

AccountTreeModel::Kind kindOf(const QModelIndex& index)
{
    return AccountTreeModel::Kind(index.data(AccountTreeModel::KindRole).toInt());
}

} // namespace

QString AccountTreeDelegate::environmentLabel(int environment)
{
    switch (Environment(environment)) {
    case Environment::Production:
        return tr("PROD");
    case Environment::Staging:
        return tr("STAGING");
    case Environment::Development:
        return tr("DEV");
    }
    return {};
}

QColor AccountTreeDelegate::environmentColor(int environment)
{
    switch (Environment(environment)) {
    case Environment::Production:
        return Theme::dangerColor();
    case Environment::Staging:
        return Theme::warningColor();
    case Environment::Development:
        return Theme::successColor();
    }
    return Theme::current().text3;
}

void AccountTreeDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    const Palette& p = Theme::current();
    option->features |= QStyleOptionViewItem::HasDecoration;
    option->decorationSize = QSize(16, 16);

    switch (kindOf(index)) {
    case AccountTreeModel::Kind::Account:
        option->icon = IconFactory::icon(QStringLiteral("cloud"), p.text2, 16);
        option->font.setBold(true);
        break;
    case AccountTreeModel::Kind::Bucket:
        option->icon = IconFactory::icon(index.data(AccountTreeModel::ExternalRole).toBool()
                                             ? QStringLiteral("link")
                                             : QStringLiteral("bucket"),
                                         p.text2, 16);
        break;
    case AccountTreeModel::Kind::Status: {
        const auto status = AccountTreeModel::Status(index.data(AccountTreeModel::StatusRole).toInt());
        const bool error = status == AccountTreeModel::Status::Error;
        option->icon = status == AccountTreeModel::Status::Loading
                           ? QIcon()
                           : IconFactory::icon(error ? QStringLiteral("warning") : QStringLiteral("info"),
                                               error ? Theme::dangerColor() : p.text3, 16);
        option->palette.setColor(QPalette::Text, error ? Theme::dangerColor() : p.text3);
        option->font.setItalic(!error);
        break;
    }
    }
}

void AccountTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (kindOf(index) != AccountTreeModel::Kind::Account) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const int env = index.data(AccountTreeModel::EnvironmentRole).toInt();
    const bool readOnly = index.data(AccountTreeModel::ReadOnlyRole).toBool();
    QFont badgeFont = opt.font;
    badgeFont.setBold(true);
    badgeFont.setPointSizeF(qMax(6.0, badgeFont.pointSizeF() - 2.0));
    const QFontMetrics bfm(badgeFont);
    const QString label = environmentLabel(env);
    const int badgeW = bfm.horizontalAdvance(label) + 2 * kBadgePadX;
    const int badgeH = bfm.height() + 2;
    const int reserved = badgeW + kBadgeGap + (readOnly ? kLockSize + 4 : 0);

    // The style paints background, icon and the name, shortened to leave
    // room for the badge.
    const QWidget* widget = opt.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    opt.text = QFontMetrics(opt.font).elidedText(opt.text, Qt::ElideRight, qMax(0, textRect.width() - reserved));
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QColor color = environmentColor(env);
    const QRect badge(opt.rect.right() - badgeW - 4, opt.rect.center().y() - badgeH / 2, badgeW, badgeH);
    QPainterPath path;
    path.addRoundedRect(QRectF(badge).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    QColor fill = color;
    fill.setAlpha(Theme::isDark() ? 50 : 30);
    painter->fillPath(path, fill);
    painter->setPen(color);
    painter->drawPath(path);
    painter->setFont(badgeFont);
    painter->drawText(badge, Qt::AlignCenter, label);

    if (readOnly) {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const QPixmap lock = IconFactory::pixmap(QStringLiteral("lock"), Theme::current().text2, kLockSize, 1.6, dpr);
        const QPoint at(badge.left() - kLockSize - 4, opt.rect.center().y() - kLockSize / 2);
        painter->drawPixmap(at, lock);
    }
    painter->restore();
}

} // namespace qbucket
