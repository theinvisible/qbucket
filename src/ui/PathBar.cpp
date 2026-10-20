#include "ui/PathBar.h"

#include "ui/AccountTreeDelegate.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStackedWidget>
#include <QToolButton>

namespace qbucket {

namespace {

// Escape leaves the edit mode; everything else goes to the line edit.
class PathEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onEscape;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape && onEscape) {
            onEscape();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QLineEdit::focusOutEvent(event);
        if (onEscape)
            onEscape();
    }
};

} // namespace

PathBar::PathBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("pathBar"));
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget;
    lay->addWidget(m_stack);

    m_crumbs = new QWidget;
    m_crumbLayout = new QHBoxLayout(m_crumbs);
    m_crumbLayout->setContentsMargins(8, 3, 8, 3);
    m_crumbLayout->setSpacing(2);
    m_stack->addWidget(m_crumbs);

    auto* edit = new PathEdit;
    edit->setObjectName(QStringLiteral("pathEdit"));
    edit->setPlaceholderText(tr("bucket/folder/…"));
    edit->onEscape = [this] { finishEdit_(false); };
    m_edit = edit;
    connect(m_edit, &QLineEdit::returnPressed, this, [this] { finishEdit_(true); });
    m_stack->addWidget(m_edit);

    clearLocation(tr("No location"));
}

void PathBar::setLocation(const QString& accountName, int environment, const Location& location)
{
    m_accountName = accountName;
    m_environment = environment;
    m_location = location;
    m_hint.clear();
    rebuild_();
}

void PathBar::clearLocation(const QString& hint)
{
    m_location = Location{};
    m_accountName.clear();
    m_hint = hint;
    rebuild_();
}

void PathBar::editPath()
{
    if (!m_location.isValid())
        return;
    m_edit->setText(m_location.bucket.isEmpty() ? QString() : m_location.bucket + u'/' + m_location.prefix);
    m_stack->setCurrentWidget(m_edit);
    m_edit->setFocus();
    m_edit->selectAll();
}

void PathBar::finishEdit_(bool accept)
{
    if (m_stack->currentWidget() != m_edit)
        return;
    m_stack->setCurrentWidget(m_crumbs);   // first: focusOut calls back in
    if (!accept)
        return;
    QString path = m_edit->text().trimmed();
    if (path.startsWith(QLatin1String("s3://")))
        path = path.mid(5);
    while (path.startsWith(u'/'))
        path.remove(0, 1);
    if (path.isEmpty())
        return;
    const qsizetype slash = path.indexOf(u'/');
    const QString bucket = slash < 0 ? path : path.left(slash);
    QString prefix = slash < 0 ? QString() : path.mid(slash + 1);
    if (!prefix.isEmpty() && !prefix.endsWith(u'/'))
        prefix += u'/';
    emit pathEntered(bucket, prefix);
}

void PathBar::rebuild_()
{
    // deleteLater: rebuild_ runs from a crumb's own clicked signal.
    while (QLayoutItem* item = m_crumbLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_stack->setCurrentWidget(m_crumbs);

    if (!m_location.isValid()) {
        auto* hint = new QLabel(m_hint);
        hint->setObjectName(QStringLiteral("pathText"));
        m_crumbLayout->addWidget(hint);
        m_crumbLayout->addStretch(1);
        return;
    }

    auto* badge = new QLabel(AccountTreeDelegate::environmentLabel(m_environment));
    badge->setObjectName(QStringLiteral("envBadge"));
    const QColor color = AccountTreeDelegate::environmentColor(m_environment);
    badge->setStyleSheet(QStringLiteral("QLabel#envBadge { color: %1; border: 1px solid %1; }").arg(color.name()));
    m_crumbLayout->addWidget(badge);

    auto* account = new QLabel(m_accountName);
    account->setObjectName(QStringLiteral("crumbAccount"));
    m_crumbLayout->addWidget(account);

    if (m_location.hasBucket()) {
        addSeparator_();
        const QStringList segments = m_location.segments();
        addCrumb_(m_location.bucket, QString(), segments.isEmpty());

        // Fold the leading folders of a deep path into a menu.
        const qsizetype hidden = qMax<qsizetype>(0, segments.size() - kVisibleSegments);
        QString prefix;
        if (hidden > 0) {
            addSeparator_();
            auto* more = new QToolButton;
            more->setObjectName(QStringLiteral("crumb"));
            more->setText(QStringLiteral("…"));
            more->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(more);
            for (qsizetype i = 0; i < hidden; ++i) {
                prefix += segments[i] + u'/';
                const QString target = prefix;
                menu->addAction(segments[i].isEmpty() ? tr("(empty name)") : segments[i], this,
                                [this, target] { emit crumbActivated(m_location.bucket, target); });
            }
            more->setMenu(menu);
            m_crumbLayout->addWidget(more);
        }
        for (qsizetype i = hidden; i < segments.size(); ++i) {
            prefix += segments[i] + u'/';
            addSeparator_();
            addCrumb_(segments[i].isEmpty() ? tr("(empty name)") : segments[i], prefix, i == segments.size() - 1);
        }
    }
    m_crumbLayout->addStretch(1);
}

void PathBar::addCrumb_(const QString& text, const QString& prefix, bool current)
{
    auto* crumb = new QToolButton;
    crumb->setObjectName(current ? QStringLiteral("crumbCurrent") : QStringLiteral("crumb"));
    crumb->setText(text);
    crumb->setToolTip(m_location.bucket + u'/' + prefix);
    crumb->setAutoRaise(true);
    connect(crumb, &QToolButton::clicked, this,
            [this, prefix] { emit crumbActivated(m_location.bucket, prefix); });
    m_crumbLayout->addWidget(crumb);
}

void PathBar::addSeparator_()
{
    auto* sep = new QLabel(QStringLiteral("›"));
    sep->setObjectName(QStringLiteral("crumbSep"));
    m_crumbLayout->addWidget(sep);
}

} // namespace qbucket
