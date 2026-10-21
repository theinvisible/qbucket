#include "ui/AccountSidebar.h"

#include "ui/ObjectBrowser.h"

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/AccountTreeModel.h"
#include "ui/AccountDialog.h"
#include "ui/AccountTreeDelegate.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QDropEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeView>
#include <QVBoxLayout>

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

void AccountSidebar::updateEmptyState_()
{
    const bool any = m_accounts && !m_accounts->accounts().isEmpty();
    m_tree->setVisible(any);
    m_empty->setVisible(!any);
}

void AccountSidebar::onCurrentChanged_(const QModelIndex& current)
{
    if (!current.isValid())
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
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }

    const QUuid id = index.data(AccountTreeModel::AccountIdRole).toUuid();
    const AccountTreeModel::Kind kind = kindOf(index);
    menu.addAction(tr("Refresh"), this, [this, index] { m_model->refresh(index); });
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
