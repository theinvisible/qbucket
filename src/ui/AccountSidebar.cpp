#include "ui/AccountSidebar.h"

#include "ui/ObjectBrowser.h"

#include "accounts/AccountStore.h"
#include "accounts/LocationStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/AccountTreeModel.h"
#include "models/BookmarkModel.h"
#include "ui/AccountDialog.h"
#include "ui/AccountTreeDelegate.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QDropEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace qbucket {

namespace {

QLabel* makeEmptyState(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("emptyState"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

AccountTreeModel::Kind kindOf(const QModelIndex& index)
{
    return AccountTreeModel::Kind(index.data(AccountTreeModel::KindRole).toInt());
}

} // namespace

AccountSidebar::AccountSidebar(AccountStore* accounts, SecretStore* secrets, AccountSessions* sessions,
                               QWidget* parent)
    : QWidget(parent),
      m_accounts(accounts),
      m_secrets(secrets),
      m_sessions(sessions)
{
    setObjectName(QStringLiteral("sidebar"));
    setMinimumWidth(180);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* header = new QLabel(tr("ACCOUNTS"));
    header->setObjectName(QStringLiteral("sectionHeader"));
    lay->addWidget(header);

    m_tree = new QTreeView;
    m_tree->setObjectName(QStringLiteral("accountTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setItemDelegate(new AccountTreeDelegate(m_tree));
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DropOnly);
    m_tree->viewport()->installEventFilter(this);
    lay->addWidget(m_tree, 1);

    m_empty = makeEmptyState(tr("No accounts yet."));
    lay->addWidget(m_empty, 1);

    m_bookmarkToggle = new QToolButton;
    m_bookmarkToggle->setObjectName(QStringLiteral("sectionToggle"));
    m_bookmarkToggle->setText(tr("BOOKMARKS"));
    m_bookmarkToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_bookmarkToggle->setArrowType(Qt::DownArrow);
    m_bookmarkToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_bookmarkToggle->setVisible(false);
    lay->addWidget(m_bookmarkToggle);
    m_bookmarkList = new QListView;
    m_bookmarkList->setObjectName(QStringLiteral("bookmarkList"));
    m_bookmarkList->setUniformItemSizes(true);
    m_bookmarkList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bookmarkList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_bookmarkList->setVisible(false);
    lay->addWidget(m_bookmarkList);
    m_bookmarkHint = new QLabel(tr("Ctrl+B bookmarks the folder you are in."));
    m_bookmarkHint->setObjectName(QStringLiteral("sectionHint"));
    m_bookmarkHint->setWordWrap(true);
    m_bookmarkHint->setVisible(false);
    lay->addWidget(m_bookmarkHint);
    connect(m_bookmarkToggle, &QToolButton::clicked, this, [this] { setBookmarksExpanded(!m_bookmarksExpanded); });

    m_addButton = new QPushButton(tr("Add Account…"));
    m_addButton->setObjectName(QStringLiteral("ghost"));
    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(10, 6, 10, 12);
    btnRow->addWidget(m_addButton);
    lay->addLayout(btnRow);

    const bool available = m_accounts && m_secrets && m_sessions;
    m_addButton->setEnabled(available);
    if (!available) {
        m_addButton->setToolTip(tr("Not available yet"));
        updateEmptyState_();
        return;
    }

    m_model = new AccountTreeModel(m_accounts, m_sessions, this);
    m_tree->setModel(m_model);
    connect(m_addButton, &QPushButton::clicked, this, &AccountSidebar::addAccountRequested);
    connect(m_accounts, &AccountStore::accountsChanged, this, &AccountSidebar::updateEmptyState_);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &AccountSidebar::showContextMenu_);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) { onCurrentChanged_(current); });
    connect(m_tree, &QTreeView::activated, this, &AccountSidebar::onActivated_);
    connect(m_secrets, &SecretStore::secretNeeded, this, [this](const QUuid& id, SecretStore::Kind kind) {
        if (kind == SecretStore::Kind::SecretAccessKey)
            askForSecret_(id);
    });

    // F5 is the main window's View → Refresh, which asks the focused side:
    // a second F5 shortcut here would make both ambiguous.
    updateEmptyState_();
}

void AccountSidebar::refreshCurrent()
{
    if (m_model)
        m_model->refresh(m_tree->currentIndex());
}

void AccountSidebar::refreshIcons()
{
    m_tree->viewport()->update();
}

void AccountSidebar::setLocationStore(LocationStore* store)
{
    if (m_locations || !store)
        return;
    m_locations = store;
    m_bookmarkModel = new BookmarkModel(store, m_accounts, this);
    m_bookmarkList->setModel(m_bookmarkModel);
    connect(m_bookmarkModel, &QAbstractItemModel::modelReset, this, &AccountSidebar::updateBookmarks_);
    connect(m_bookmarkList, &QListView::activated, this, [this](const QModelIndex& index) {
        emit bookmarkActivated(m_bookmarkModel->bookmarkAt(index.row()).location);
    });
    // A single click opens a bookmark, as in a file manager's places.
    connect(m_bookmarkList, &QListView::clicked, this, [this](const QModelIndex& index) {
        emit bookmarkActivated(m_bookmarkModel->bookmarkAt(index.row()).location);
    });
    connect(m_bookmarkList, &QWidget::customContextMenuRequested, this, &AccountSidebar::showBookmarkMenu_);
    m_bookmarkToggle->setVisible(true);
    updateBookmarks_();
}

void AccountSidebar::setBookmarksExpanded(bool expanded)
{
    if (m_bookmarksExpanded == expanded)
        return;
    m_bookmarksExpanded = expanded;
    updateBookmarks_();
    emit bookmarksExpandedChanged(expanded);
}

void AccountSidebar::updateBookmarks_()
{
    if (!m_locations)
        return;
    constexpr int kVisibleRows = 8;
    const int rows = m_bookmarkModel->rowCount();
    m_bookmarkToggle->setArrowType(m_bookmarksExpanded ? Qt::DownArrow : Qt::RightArrow);
    m_bookmarkList->setVisible(m_bookmarksExpanded && rows > 0);
    m_bookmarkHint->setVisible(m_bookmarksExpanded && rows == 0);
    if (rows > 0) {
        const int rowHeight = std::max(m_bookmarkList->sizeHintForRow(0), fontMetrics().height() + 6);
        m_bookmarkList->setFixedHeight(std::min(rows, kVisibleRows) * rowHeight + 2 * m_bookmarkList->frameWidth() + 4);
    }
}

void AccountSidebar::showBookmarkMenu_(const QPoint& pos)
{
    const QModelIndex index = m_bookmarkList->indexAt(pos);
    if (!index.isValid())
        return;
    const Bookmark b = m_bookmarkModel->bookmarkAt(index.row());
    QMenu menu(this);
    menu.addAction(tr("Open"), this, [this, b] { emit bookmarkActivated(b.location); });
    menu.addAction(tr("Rename…"), this, [this, id = b.id] { renameBookmark_(id); });
    menu.addSeparator();
    menu.addAction(tr("Remove Bookmark"), this, [this, id = b.id] { m_locations->removeBookmark(id); });
    menu.exec(m_bookmarkList->viewport()->mapToGlobal(pos));
}

void AccountSidebar::renameBookmark_(const QUuid& id)
{
    const std::optional<Bookmark> b = m_locations->bookmark(id);
    if (!b)
        return;
    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(tr("Rename Bookmark"));
    dlg->setLabelText(tr("Name:"));
    dlg->setTextValue(b->name);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, id](const QString& name) {
        m_locations->renameBookmark(id, name);
    });
    dlg->open();
}

void AccountSidebar::setCurrentLocation(const Location& location)
{
    if (!m_model || !location.isValid())
        return;
    QModelIndex target = location.hasBucket() ? m_model->bucketIndex(location.account, location.bucket) : QModelIndex();
    if (!target.isValid())
        target = m_model->accountIndex(location.account);
    if (!target.isValid() || target == m_tree->currentIndex())
        return;
    m_following = true;
    if (target.parent().isValid())
        m_tree->expand(target.parent());
    m_tree->setCurrentIndex(target);
    m_tree->scrollTo(target);
    m_following = false;
}

void AccountSidebar::updateEmptyState_()
{
    const bool any = m_accounts && !m_accounts->accounts().isEmpty();
    m_tree->setVisible(any);
    m_empty->setVisible(!any);
}

void AccountSidebar::onCurrentChanged_(const QModelIndex& current)
{
    if (!current.isValid() || m_following)
        return;
    const QUuid id = current.data(AccountTreeModel::AccountIdRole).toUuid();
    if (kindOf(current) == AccountTreeModel::Kind::Bucket)
        emit bucketSelected(id, current.data(AccountTreeModel::BucketRole).toString());
    else
        emit accountSelected(id);
}

void AccountSidebar::onActivated_(const QModelIndex& index)
{
    // Enter / double-click on a failed listing tries again.
    if (kindOf(index) == AccountTreeModel::Kind::Status
        && AccountTreeModel::Status(index.data(AccountTreeModel::StatusRole).toInt())
               == AccountTreeModel::Status::Error)
        m_model->refresh(index);
}

void AccountSidebar::showContextMenu_(const QPoint& pos)
{
    const QModelIndex index = m_tree->indexAt(pos);
    QMenu menu(this);
    if (!index.isValid()) {
        menu.addAction(tr("Add Account…"), this, &AccountSidebar::addAccountRequested);
        menu.addAction(tr("Import Accounts…"), this, &AccountSidebar::importAccountsRequested);
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }

    const QUuid id = index.data(AccountTreeModel::AccountIdRole).toUuid();
    const AccountTreeModel::Kind kind = kindOf(index);
    menu.addAction(tr("Refresh"), this, [this, index] { m_model->refresh(index); });
    if (kind == AccountTreeModel::Kind::Bucket) {
        const QString bucket = index.data(AccountTreeModel::BucketRole).toString();
        menu.addAction(tr("Properties…"), this, [this, id, bucket] { emit bucketPropertiesRequested(id, bucket); });
    }
    menu.addSeparator();
    menu.addAction(tr("New Bucket…"), this, [this, id] { emit createBucketRequested(id); });
    if (kind == AccountTreeModel::Kind::Bucket) {
        const QString bucket = index.data(AccountTreeModel::BucketRole).toString();
        menu.addAction(tr("Delete Bucket “%1”…").arg(bucket), this,
                       [this, id, bucket] { emit deleteBucketRequested(id, bucket); });
    }
    if (kind == AccountTreeModel::Kind::Bucket && index.data(AccountTreeModel::ExternalRole).toBool()) {
        const QString bucket = index.data(AccountTreeModel::BucketRole).toString();
        menu.addAction(tr("Remove “%1” from the List").arg(bucket), this,
                       [this, id, bucket] { removeExternalBucket_(id, bucket); });
    }
    menu.addSeparator();
    menu.addAction(tr("Add External Bucket…"), this, [this, id] { addExternalBucket_(id); });
    menu.addAction(tr("Edit Account…"), this, [this, id] { editAccount_(id); });
    menu.addSeparator();
    menu.addAction(tr("Remove Account…"), this, [this, id] { removeAccount_(id); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void AccountSidebar::editAccount_(const QUuid& id)
{
    const std::optional<Account> account = m_accounts->account(id);
    if (!account)
        return;
    auto* dlg = new AccountDialog(m_accounts, m_secrets, account, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &AccountDialog::accountSaved, this, [this](const Account& a, bool sessionOnly) {
        // The secret may have changed; connection changes restart anyway.
        m_sessions->reconnect(a.id);
        emit statusMessage(sessionOnly
                               ? tr("Account “%1” saved — its secret is kept for this session only.").arg(a.name)
                               : tr("Account “%1” saved.").arg(a.name),
                           8000);
    });
    dlg->open();
}

void AccountSidebar::removeAccount_(const QUuid& id)
{
    const std::optional<Account> account = m_accounts->account(id);
    if (!account)
        return;
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Remove Account"),
                                tr("Remove the account “%1” from QBucket?").arg(account->name),
                                QMessageBox::Cancel, this);
    box->setInformativeText(tr("Its secret is deleted from the keychain. Nothing on the server is changed."));
    QPushButton* remove = box->addButton(tr("Remove"), QMessageBox::DestructiveRole);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, remove, id, name = account->name] {
        if (box->clickedButton() != remove)
            return;
        m_secrets->removeAll(id, this, [this, id, name](const SecretStore::Result& r) {
            m_accounts->remove(id);
            emit statusMessage(r.ok() ? tr("Account “%1” removed.").arg(name)
                                      : tr("Account “%1” removed, but its secret could not be deleted from "
                                           "the keychain: %2").arg(name, r.message),
                               8000);
        });
    });
    box->open();
}

void AccountSidebar::addExternalBucket_(const QUuid& id)
{
    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(tr("Add External Bucket"));
    dlg->setLabelText(tr("Bucket name (for buckets these credentials may use but not list):"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, id](const QString& text) {
        const QString name = text.trimmed();
        static const QRegularExpression invalid(QStringLiteral("[\\s/\\\\]"));
        std::optional<Account> account = m_accounts->account(id);
        if (!account || name.isEmpty())
            return;
        if (invalid.match(name).hasMatch() || name.size() > 255) {
            emit statusMessage(tr("“%1” is not a valid bucket name.").arg(name), 8000);
            return;
        }
        if (account->externalBuckets.contains(name))
            return;
        account->externalBuckets.append(name);
        m_accounts->save(*account);
    });
    dlg->open();
}

void AccountSidebar::removeExternalBucket_(const QUuid& id, const QString& bucket)
{
    std::optional<Account> account = m_accounts->account(id);
    if (!account)
        return;
    account->externalBuckets.removeAll(bucket);
    m_accounts->save(*account);
}

void AccountSidebar::askForSecret_(const QUuid& id)
{
    const std::optional<Account> account = m_accounts->account(id);
    if (!account || m_askingSecret.contains(id))
        return;
    m_askingSecret.insert(id);

    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(tr("Secret Access Key"));
    dlg->setLabelText(m_secrets->isSessionOnly()
                          ? tr("No keychain is available. Secret access key for “%1” "
                               "(kept in memory until QBucket quits):").arg(account->name)
                          : tr("The keychain has no secret for “%1”. Secret access key "
                               "(kept for this session; edit the account to store it):").arg(account->name));
    dlg->setTextEchoMode(QLineEdit::Password);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, id](const QString& secret) {
        if (secret.isEmpty())
            return;
        m_secrets->setSessionSecret(id, SecretStore::Kind::SecretAccessKey, secret);
        m_sessions->reconnect(id);   // the old session holds the failed lookup
        if (m_model)
            m_model->refresh(m_model->accountIndex(id));
    });
    connect(dlg, &QDialog::finished, this, [this, id] { m_askingSecret.remove(id); });
    dlg->open();
}

bool AccountSidebar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_tree->viewport())
        return QWidget::eventFilter(watched, event);
    const QEvent::Type t = event->type();
    if (t != QEvent::DragEnter && t != QEvent::DragMove && t != QEvent::Drop)
        return QWidget::eventFilter(watched, event);
    auto* drop = static_cast<QDropEvent*>(event);
    const QModelIndex at = m_tree->indexAt(drop->position().toPoint());
    // Only onto a bucket of an account that may write; the gate checks again.
    const bool bucket = at.isValid()
        && at.data(AccountTreeModel::KindRole).toInt() == int(AccountTreeModel::Kind::Bucket)
        && !at.data(AccountTreeModel::ReadOnlyRole).toBool();
    if (const std::optional<ObjectSelection> sel = ObjectSelection::fromMimeData(drop->mimeData())) {
        const QUuid account = at.data(AccountTreeModel::AccountIdRole).toUuid();
        const QString target = at.data(AccountTreeModel::BucketRole).toString();
        // Same account only (between accounts: later); not back onto the root it came from.
        const bool ok = bucket && account == sel->account && !(target == sel->bucket && sel->base.isEmpty());
        if (t != QEvent::DragEnter && !ok) {
            drop->ignore();
            return true;
        }
        const bool move = drop->modifiers() & Qt::ShiftModifier;
        drop->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
        drop->accept();
        if (t == QEvent::Drop && ok)
            emit objectsDroppedOnBucket(account, target, *sel, move);
        return true;
    }
    const QStringList paths = ObjectBrowser::localPaths(drop->mimeData());
    if (paths.isEmpty() || (t != QEvent::DragEnter && !bucket)) {
        drop->ignore();
        return true;
    }
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (t == QEvent::Drop && bucket) {
        emit filesDroppedOnBucket(at.data(AccountTreeModel::AccountIdRole).toUuid(),
                                  at.data(AccountTreeModel::BucketRole).toString(), paths);
    }
    return true;
}

} // namespace qbucket
