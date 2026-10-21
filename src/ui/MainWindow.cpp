#include "ui/MainWindow.h"

#include "accounts/AccountStore.h"
#include "accounts/CapabilityStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ActivityLog.h"
#include "controllers/CapabilityController.h"
#include "controllers/CopyJob.h"
#include "controllers/ConnectionTester.h"
#include "controllers/CreateFolderJob.h"
#include "controllers/DeleteJob.h"
#include "controllers/EditObjectsJob.h"
#include "controllers/RenameJob.h"
#include "controllers/Job.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/PresignService.h"
#include "controllers/DiscardTransfersJob.h"
#include "controllers/DownloadJob.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"
#include "ui/AccountSidebar.h"
#include "ui/CapabilitiesDialog.h"
#include "ui/EditMetadataDialog.h"
#include "ui/IncompleteUploadsDialog.h"
#include "ui/LogPanel.h"
#include "ui/OperationPreviewDialog.h"
#include "ui/PresignDialog.h"
#include "ui/DetailPane.h"
#include "controllers/BrowseController.h"
#include "ui/ObjectBrowser.h"
#include "controllers/SettingsController.h"
#include "ui/AccountDialog.h"
#include "ui/IconFactory.h"
#include "ui/SettingsDialog.h"
#include "ui/TransferPanel.h"
#include "models/TransferModel.h"
#include "ui/Theme.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/transfer/ResumeStore.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>

namespace qbucket {
namespace {

// On Windows, QKeySequence::Quit and QKeySequence::Preferences don't resolve
// to a key chord — they map to the named system keys Qt::Key_Exit /
// Qt::Key_Settings, whose text is the bare word "Exit" / "Settings". A QMenu
// paints that word in the item's shortcut column, and with the German Qt
// translation loaded the entry looks doubled ("Einstellungen … Einstellungen").
// Keep the shortcut only when it carries a modifier or is a function key
// (Linux's Ctrl+Q, F5); drop the bare named-key sequences. (xyz-profiler
// lesson — don't go back to a bare setShortcut() for standard keys.)
void setStandardShortcut(QAction* action, QKeySequence::StandardKey key)
{
    const QString portable = QKeySequence(key).toString(QKeySequence::PortableText);
    static const QRegularExpression fnKey(QStringLiteral("^F\\d+$"));
    if (portable.contains(QLatin1Char('+')) || fnKey.match(portable).hasMatch())
        action->setShortcut(key);
}

QToolButton* makeIconButton(const QString& tooltip)
{
    auto* btn = new QToolButton;
    btn->setObjectName(QStringLiteral("tbIcon"));
    btn->setToolTip(tooltip);
    return btn;
}

QLabel* makeEmptyState(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("emptyState"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

QPixmap statusDot(const QColor& color, qreal dpr)
{
    constexpr int kSize = 10;
    QPixmap pm(QSize(kSize, kSize) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, kSize - 2, kSize - 2));
    return pm;
}

constexpr int kSidebarWidth = 250;
constexpr int kDetailWidth  = 400;

} // namespace

MainWindow::MainWindow(SettingsController* settings, ThemeManager* theme,
                       AccountStore* accounts, SecretStore* secrets, QWidget* parent)
    : QMainWindow(parent),
      m_settings(settings),
      m_theme(theme),
      m_accounts(accounts),
      m_secrets(secrets)
{
    setWindowTitle(QStringLiteral("QBucket"));
    m_activity = new ActivityLog(this);
    if (m_accounts && m_secrets) {
        m_sessions = new AccountSessions(m_accounts, m_secrets, this);
        m_capStore = new CapabilityStore(this);
        m_capabilities = new CapabilityController(m_accounts, m_sessions, m_capStore, m_activity, this);
        m_journal = new Journal(this);
        m_operations = new OperationController(m_accounts, m_sessions, m_capabilities, m_journal, m_activity, this);
        m_transfers = new TransferService(this);
        m_operations->setTransfers(m_transfers);
        m_presign = new PresignService(m_operations, this);
        const auto applyTransferSettings = [this] {
            m_sessions->setTransferLanes(m_settings->transferThreads());
            m_transfers->setMaxParallelParts(m_settings->transferParallelParts());
        };
        applyTransferSettings();
        connect(m_settings, &SettingsController::transferSettingsChanged, this, applyTransferSettings);
    }

    buildMenuBar_();
    buildToolBar_();
    buildCentralWidget_();
    buildTransferDock_();
    buildStatusBar_();
    connectSettings_();
    restoreWindowState_();

    applyBrowseMode_();
    applyThemeChoice_();
    refreshIcons_();

    const bool accountsAvailable = m_sessions != nullptr;
    m_actAddAccount->setEnabled(accountsAvailable);
    if (accountsAvailable) {
        connect(m_actAddAccount, &QAction::triggered, this, &MainWindow::showAddAccountDialog_);
        connect(m_sidebar, &AccountSidebar::addAccountRequested, this, &MainWindow::showAddAccountDialog_);
        connect(m_secrets, &SecretStore::sessionOnlyModeEntered, this, [this](const QString& reason) {
            statusBar()->showMessage(
                tr("No system keychain available — secrets are kept for this session only (%1).").arg(reason));
        });
    }
    connect(m_sidebar, &AccountSidebar::statusMessage, this,
            [this](const QString& text, int timeoutMs) { statusBar()->showMessage(text, timeoutMs); });
    connect(m_sidebar, &AccountSidebar::bucketSelected, m_browser, &ObjectBrowser::openBucket);
    // Drops always show the preview: a drop is easily an accident.
    connect(m_sidebar, &AccountSidebar::filesDroppedOnBucket, this,
            [this](const QUuid& account, const QString& bucket, const QStringList& paths) {
                startUploadTo_(account, bucket, QString(), paths, true);
            });
    connect(m_browser, &ObjectBrowser::filesDropped, this, [this](const QStringList& paths, const QString& prefix) {
        const Location here = m_browser->navigation()->current();
        startUploadTo_(here.account, here.bucket, prefix, paths, true);
    });
    // Objects: a paste moves what was cut; drops copy (Shift: move).
    connect(m_browser, &ObjectBrowser::pasteRequested, this, [this](const ObjectSelection& sel, const QString& prefix) {
        const Location here = m_browser->navigation()->current();
        startCopy_(sel, here.account, here.bucket, prefix, sel.intent == ObjectSelection::Intent::Cut);
    });
    connect(m_browser, &ObjectBrowser::objectsDropped, this, [this](const ObjectSelection& sel, const QString& prefix, bool move) {
        const Location here = m_browser->navigation()->current();
        startCopy_(sel, here.account, here.bucket, prefix, move);
    });
    connect(m_sidebar, &AccountSidebar::objectsDroppedOnBucket, this,
            [this](const QUuid& account, const QString& bucket, const ObjectSelection& sel, bool move) {
                startCopy_(sel, account, bucket, QString(), move);
            });
    connect(m_sidebar, &AccountSidebar::accountSelected, this, [this](const QUuid& id) {
        m_browser->navigation()->navigate({id, {}, {}});
    });
    connectBrowser_();
    connectOperations_();
    if (m_transfers)
        QTimer::singleShot(0, this, &MainWindow::offerResume_);
}

void MainWindow::offerResume_()
{
    m_transfers->jobs()->load(this, [this](const QList<TransferJobStore::Record>& records) {
        if (records.isEmpty() || !m_operations)
            return;
        QStringList lines;
        for (const TransferJobStore::Record& r : records) {
            const std::optional<Account> a = m_accounts->account(r.account);
            lines.append(QStringLiteral("%1 (%2)").arg(r.title, a ? a->name : tr("account removed")));
        }
        auto* box = new QMessageBox(QMessageBox::Question, tr("Unfinished Transfers"),
                                    tr("%n transfer(s) did not finish in the last session. Continue where they stopped?",
                                       nullptr, int(records.size())),
                                    QMessageBox::NoButton, this);
        box->setDetailedText(lines.join(u'\n'));
        QPushButton* resume = box->addButton(tr("Resume"), QMessageBox::AcceptRole);
        QPushButton* discard = box->addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box->addButton(tr("Later"), QMessageBox::RejectRole);
        box->setDefaultButton(resume);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QMessageBox::buttonClicked, this, [this, records, resume, discard](QAbstractButton* b) {
            for (const TransferJobStore::Record& r : records) {
                const std::optional<Account> a = m_accounts->account(r.account);
                if (!a) {
                    if (b == discard)
                        m_transfers->jobs()->remove(r.id);
                    continue;
                }
                if (b == resume) {
                    if (r.kind == Job::TransferKind::Upload)
                        m_operations->start(new UploadJob(*a, r));
                    else
                        m_operations->start(new DownloadJob(*a, r));
                } else if (b == discard) {
                    m_operations->start(new DiscardTransfersJob(*a, r));
                }
            }
        });
        box->open();
    });
}

// ---------------------------------------------------------------------------
// Operations: preview, progress, report (PLAN.md §12.2)
// ---------------------------------------------------------------------------
void MainWindow::connectOperations_()
{
    if (!m_operations)
        return;
    // Read-only switched, capabilities learned: what may be done changes.
    connect(m_accounts, &AccountStore::accountsChanged, this, &MainWindow::updateMutationActions_);
    connect(m_capabilities, &CapabilityController::capabilitiesUpdated, this, &MainWindow::updateMutationActions_);
    connect(m_operations, &OperationController::previewNeeded, this, [this](Job* job) {
        auto* dlg = new OperationPreviewDialog(job, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });
    connect(m_operations, &OperationController::jobStarted, this, [this](Job* job) {
        connect(job, &Job::progressChanged, this, &MainWindow::updateJobStatus_);
        connect(job, &Job::phaseChanged, this, &MainWindow::updateJobStatus_);
        updateJobStatus_();
    });
    connect(m_operations, &OperationController::jobFinished, this, [this](Job* job) {
        QMetaObject::invokeMethod(this, &MainWindow::updateJobStatus_, Qt::QueuedConnection);
        showJobReport_(job);
        // What the job changed is listed afresh.
        const Location here = m_browser->navigation()->current();
        if (job->outcome() != Job::Outcome::Refused && job->phase() == Job::Phase::Finished
            && here.account == job->account().id && job->buckets().contains(here.bucket))
            m_browser->refresh();
    });
    connect(m_jobCancel, &QToolButton::clicked, this, [this] {
        for (Job* job : m_operations->jobs())
            job->cancel();
    });
    connect(m_journal, &Journal::writeFailed, this, [this](const QString& message) {
        m_activity->add(ActivityLog::Level::Error, tr("The operation journal could not be written: %1").arg(message));
    });
}

void MainWindow::updateMutationActions_()
{
    const Location here = m_browser->navigation()->current();
    OperationController::Check folder{false, tr("Open a bucket first.")};
    if (m_operations && here.hasBucket())
        folder = m_operations->check(here.account, here.bucket, {}, false);
    m_actNewFolder->setEnabled(folder.allowed);
    m_newFolderBtn->setEnabled(folder.allowed);
    // Uploading may overwrite (after the preview): the account must allow writes.
    for (QAction* a : {m_actUploadFiles, m_actUploadFolder}) {
        if (!a)
            continue;
        a->setEnabled(folder.allowed);
        a->setToolTip(folder.allowed ? tr("Upload into s3://%1/%2").arg(here.bucket, here.prefix) : folder.reason);
    }
    m_uploadBtn->setEnabled(folder.allowed);
    m_browser->setUploadsAllowed(folder.allowed);
    m_uploadBtn->setToolTip(folder.allowed ? tr("Upload files or folders to the current location") : folder.reason);
    m_actNewFolder->setToolTip(folder.allowed ? tr("Create a folder here") : folder.reason);
    m_newFolderBtn->setToolTip(folder.allowed ? tr("New folder") : folder.reason);
    // Deleting needs no feature (BatchDelete or one by one) — only the account.
    OperationController::Check del{false, tr("Open a bucket first.")};
    if (m_operations && here.hasBucket())
        del = m_operations->check(here.account, here.bucket, {}, true);
    m_browser->setMutationsAllowed(del.allowed, del.reason);
    // A choice of storage classes (one only: nothing to choose).
    const std::optional<Account> acct = m_accounts && here.isValid() ? m_accounts->account(here.account) : std::nullopt;
    const QStringList classes =
        acct ? s3::ProviderRegistry::builtin().findOrGeneric(acct->providerId)->storageClasses() : QStringList();
    if (classes.size() == 1)
        m_browser->setStorageClassesAllowed(false, tr("This provider has one storage class only (%1).").arg(classes.first()));
    else
        m_browser->setStorageClassesAllowed(acct.has_value(), QString());
    OperationController::Check read{false, tr("Open a bucket first.")};
    if (m_operations && here.hasBucket())
        read = m_operations->check(here.account, here.bucket, {}, false, false);
    m_browser->setDownloadsAllowed(read.allowed, read.reason);
    // The versions view: wherever the bucket can list versions.
    OperationController::Check versions{false, tr("Open a bucket first.")};
    if (m_operations && here.hasBucket())
        versions = m_operations->check(here.account, here.bucket, {s3::Feature::ObjectVersionsListing}, false, false);
    const QString versionsTip = versions.allowed ? tr("Every version and delete marker of the objects here") : versions.reason;
    m_actVersions->setEnabled(versions.allowed);
    m_actVersions->setToolTip(versionsTip);
    for (QAbstractButton* btn : m_browseSeg->buttons()) {
        if (btn->property("browseMode").toString() == QLatin1String("versions")) {
            btn->setEnabled(versions.allowed);
            btn->setToolTip(versionsTip);
        }
    }
}

void MainWindow::startCopy_(const ObjectSelection& sel, const QUuid& accountId, const QString& bucket,
                            const QString& prefix, bool move)
{
    if (!m_operations || sel.isEmpty())
        return;
    if (sel.account != accountId) {
        statusBar()->showMessage(tr("Copying between accounts comes in a later version."), 10000);
        return;
    }
    const std::optional<Account> account = m_accounts ? m_accounts->account(accountId) : std::nullopt;
    if (!account)
        return;
    QList<CopyJob::Item> items;
    for (const ObjectSelection::Item& i : sel.items)
        items.append({i.key, i.folder});
    auto* job = new CopyJob(*account, sel.bucket, items, sel.base, bucket, prefix,
                            move ? CopyJob::Mode::Move : CopyJob::Mode::Copy, m_operations->planDir());
    if (move && sel.intent == ObjectSelection::Intent::Cut) {
        // What was cut is gone once moved: the clipboard lets go of it.
        connect(job, &Job::finished, this, [job, sel] {
            if (job->outcome() != Job::Outcome::Succeeded)
                return;
            const std::optional<ObjectSelection> now = ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData());
            if (now && now->intent == ObjectSelection::Intent::Cut && now->bucket == sel.bucket && now->base == sel.base)
                QGuiApplication::clipboard()->clear();
        });
    }
    m_operations->start(job);
}

void MainWindow::editMetadata_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    const QModelIndexList rows = m_browser->selectedRows();
    if (!m_operations || !account || !here.hasBucket() || rows.isEmpty())
        return;
    QList<EditObjectsJob::Item> items;
    for (const QModelIndex& r : rows)
        items.append({r.data(ObjectTableModel::KeyRole).toString(), r.data(ObjectTableModel::IsFolderRole).toBool()});
    const auto start = [this, account, here, items](const EditObjectsJob::ChangeSpec& change) {
        m_operations->start(new EditObjectsJob(*account, here.bucket, items, change, m_operations->planDir()));
    };
    if (items.size() > 1 || items.first().folder) {
        auto* dlg = new EditMetadataDialog(int(items.size()), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [dlg, start] { start(dlg->change()); });
        dlg->open();
        return;
    }
    // One object: its current values, fresh.
    const s3::Result<s3::Session*> session = m_sessions->session(here.account);
    if (!session)
        return;
    const QString key = items.first().key;
    (*session)->headObject(here.bucket, key, QString(), this, [this, key, start](const s3::Result<s3::ObjectMeta>& meta) {
        if (!meta) {
            statusBar()->showMessage(tr("Could not read “%1”: %2").arg(key, ConnectionTester::describe(meta.error())), 10000);
            return;
        }
        auto* dlg = new EditMetadataDialog(key, *meta, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [dlg, start] { start(dlg->change()); });
        dlg->open();
    });
}

void MainWindow::presign_()
{
    const Location here = m_browser->navigation()->current();
    if (!m_presign || !here.hasBucket())
        return;
    QStringList keys;
    int folders = 0;
    for (const QModelIndex& r : m_browser->selectedRows()) {
        if (r.data(ObjectTableModel::IsFolderRole).toBool())
            ++folders;
        else
            keys.append(r.data(ObjectTableModel::KeyRole).toString());
    }
    if (keys.isEmpty())
        return;
    auto* dlg = new PresignDialog(m_presign, here.account, here.bucket, keys, folders, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->open();
}

void MainWindow::changeStorageClass_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    const QModelIndexList rows = m_browser->selectedRows();
    if (!m_operations || !account || !here.hasBucket() || rows.isEmpty())
        return;
    QList<EditObjectsJob::Item> items;
    for (const QModelIndex& r : rows)
        items.append({r.data(ObjectTableModel::KeyRole).toString(), r.data(ObjectTableModel::IsFolderRole).toBool()});
    QStringList classes = s3::ProviderRegistry::builtin().findOrGeneric(account->providerId)->storageClasses();
    const bool known = !classes.isEmpty();
    if (!known) {
        // Not known for this server: STANDARD and the classes the listing shows; others may be typed.
        classes = {QStringLiteral("STANDARD")};
        for (int row = 0; row < m_browser->model()->rowCount(); ++row) {
            const QString c = m_browser->model()->index(row, 0).data(ObjectTableModel::StorageClassRole).toString();
            if (!c.isEmpty() && !classes.contains(c))
                classes.append(c);
        }
    }
    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(tr("Change Storage Class"));
    dlg->setLabelText(known ? tr("New storage class for the selection:")
                            : tr("New storage class for the selection (not known for this server — the server refuses "
                                 "one it doesn't have, nothing is changed then):"));
    dlg->setComboBoxItems(classes);
    dlg->setComboBoxEditable(!known);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, account, here, items](const QString& value) {
        const QString cls = value.trimmed().toUpper();
        static const QRegularExpression valid(QStringLiteral("^[A-Z0-9_]+$"));
        if (!valid.match(cls).hasMatch()) {
            statusBar()->showMessage(tr("“%1” is not a storage class name.").arg(value), 10000);
            return;
        }
        EditObjectsJob::ChangeSpec change;
        change.storageClass = cls;
        m_operations->start(new EditObjectsJob(*account, here.bucket, items, change, m_operations->planDir()));
    });
    dlg->open();
}

void MainWindow::renameSelection_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    const QModelIndexList rows = m_browser->selectedRows();
    if (!m_operations || !account || !here.hasBucket() || rows.size() != 1)
        return;
    const QString key = rows.first().data(ObjectTableModel::KeyRole).toString();
    const bool folder = rows.first().data(ObjectTableModel::IsFolderRole).toBool();
    const QString name = rows.first().data(ObjectTableModel::NameRole).toString();
    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(folder ? tr("Rename Folder") : tr("Rename"));
    dlg->setLabelText(tr("New name for “%1”:").arg(name));
    dlg->setTextValue(name);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, here, account, key, folder](const QString& newName) {
        if (const QString problem = CreateFolderJob::validateName(newName); !problem.isEmpty()) {
            statusBar()->showMessage(tr("Not renamed: %1").arg(problem), 10000);
            return;
        }
        m_operations->start(new RenameJob(*account, here.bucket, key, folder, newName, m_operations->planDir()));
    });
    dlg->open();
}

void MainWindow::deleteSelection_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    if (!m_operations || !account || !here.hasBucket())
        return;
    QList<DeleteJob::Item> items;
    for (const QModelIndex& row : m_browser->selectedRows())
        items.append({row.data(ObjectTableModel::KeyRole).toString(), row.data(ObjectTableModel::IsFolderRole).toBool()});
    if (items.isEmpty())
        return;
    m_operations->start(new DeleteJob(*account, here.bucket, items, m_operations->planDir()));
}

void MainWindow::chooseUploadFiles_()
{
    auto* dlg = new QFileDialog(this, tr("Upload Files"));
    dlg->setFileMode(QFileDialog::ExistingFiles);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::filesSelected, this, [this](const QStringList& files) {
        // Chosen in the dialog: a single file without conflicts goes ahead.
        startUpload_(files, files.size() != 1);
    });
    dlg->open();
}

void MainWindow::chooseUploadFolder_()
{
    auto* dlg = new QFileDialog(this, tr("Upload Folder"));
    dlg->setFileMode(QFileDialog::Directory);
    dlg->setOption(QFileDialog::ShowDirsOnly);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::filesSelected, this, [this](const QStringList& dirs) { startUpload_(dirs, true); });
    dlg->open();
}

void MainWindow::startUpload_(const QStringList& paths, bool alwaysConfirm)
{
    const Location here = m_browser->navigation()->current();
    if (here.hasBucket())
        startUploadTo_(here.account, here.bucket, here.prefix, paths, alwaysConfirm);
}

void MainWindow::startUploadTo_(const QUuid& accountId, const QString& bucket, const QString& prefix,
                                const QStringList& paths, bool alwaysConfirm)
{
    const std::optional<Account> account = m_accounts ? m_accounts->account(accountId) : std::nullopt;
    if (!m_operations || !account || bucket.isEmpty() || paths.isEmpty())
        return;
    UploadJob::Options options;
    options.alwaysConfirm = alwaysConfirm;
    m_operations->start(new UploadJob(*account, bucket, prefix, paths, m_operations->planDir(), options));
}

void MainWindow::downloadSelection_(const QString& folder)
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    if (!m_operations || !account || !here.hasBucket())
        return;
    QList<DownloadJob::Item> items;
    for (const QModelIndex& row : m_browser->selectedRows())
        items.append({row.data(ObjectTableModel::KeyRole).toString(), row.data(ObjectTableModel::IsFolderRole).toBool(), {}});
    if (items.isEmpty())
        return;
    DownloadJob::Options options;
    // Straight into the download folder: one object without a local
    // conflict needs no preview. A chosen folder was a deliberate step too.
    options.alwaysConfirm = items.size() != 1 || items.first().folder;
    const QString target = folder.isEmpty() ? m_settings->downloadDirectory() : folder;
    m_operations->start(new DownloadJob(*account, here.bucket, items, target, m_operations->planDir(), options));
}

void MainWindow::newFolder_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    if (!m_operations || !account || !here.hasBucket())
        return;
    auto* dlg = new QInputDialog(this);
    dlg->setWindowTitle(tr("New Folder"));
    dlg->setLabelText(tr("Name of the new folder in s3://%1/%2:").arg(here.bucket, here.prefix));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, here, account](const QString& name) {
        if (const QString problem = CreateFolderJob::validateName(name); !problem.isEmpty()) {
            statusBar()->showMessage(tr("No folder created: %1").arg(problem), 10000);
            return;
        }
        m_operations->start(new CreateFolderJob(*account, here.bucket, here.prefix, name));
    });
    dlg->open();
}

void MainWindow::updateJobStatus_()
{
    const QList<Job*> running = m_operations ? m_operations->jobs() : QList<Job*>{};
    QList<Job*> busy;
    for (Job* j : running) {
        if (j->phase() == Job::Phase::Planning || j->phase() == Job::Phase::Executing
            || j->phase() == Job::Phase::Verifying)
            busy.append(j);
    }
    m_jobLabel->setVisible(!busy.isEmpty());
    m_jobCancel->setVisible(!busy.isEmpty());
    if (busy.isEmpty())
        return;
    const Job* job = busy.first();
    QString step;
    switch (job->phase()) {
    case Job::Phase::Planning:
        step = tr("planning");
        break;
    case Job::Phase::Verifying:
        step = tr("verifying");
        break;
    default:
        step = job->progressTotal() > 0 ? tr("%1 of %2").arg(job->progressDone()).arg(job->progressTotal())
                                        : tr("running");
        break;
    }
    m_jobLabel->setText(busy.size() > 1 ? tr("%1 (%2) and %n more", nullptr, int(busy.size() - 1)).arg(job->title(), step)
                                        : tr("%1 (%2)").arg(job->title(), step));
}

void MainWindow::showJobReport_(Job* job)
{
    const QString summary = OperationController::summary(*job);
    const Job::Outcome outcome = job->outcome();
    if (outcome == Job::Outcome::Succeeded || outcome == Job::Outcome::Cancelled) {
        statusBar()->showMessage(summary, 10000);
        return;
    }
    auto* box = new QMessageBox(outcome == Job::Outcome::Refused ? QMessageBox::Information : QMessageBox::Warning,
                                job->title(), summary, QMessageBox::Close, this);
    QStringList details = job->report().failures;
    if (!job->report().verification.isEmpty())
        details.prepend(job->report().verification);
    details += job->report().notes;
    if (!details.isEmpty())
        box->setDetailedText(details.join(u'\n'));
    // "Retry Failed": a new download of exactly the objects that failed.
    if (const auto* down = qobject_cast<const DownloadJob*>(job); down && !down->failedItems().isEmpty()) {
        QPushButton* retry = box->addButton(tr("Retry Failed"), QMessageBox::ActionRole);
        const QList<DownloadJob::Item> items = down->failedItems();
        const Account account = down->account();
        const QString bucket = down->bucket();
        const QString target = down->targetDir();
        connect(box, &QMessageBox::buttonClicked, this, [this, retry, items, account, bucket, target](QAbstractButton* b) {
            if (b == retry && m_operations)
                m_operations->start(new DownloadJob(account, bucket, items, target, m_operations->planDir(), {}));
        });
    }
    // "Retry Failed": a new upload of exactly the files that failed.
    if (const auto* up = qobject_cast<const UploadJob*>(job); up && !up->failedItems().isEmpty()) {
        QPushButton* retry = box->addButton(tr("Retry Failed"), QMessageBox::ActionRole);
        const QList<UploadJob::Item> items = up->failedItems();
        const Account account = up->account();
        const QString bucket = up->bucket();
        connect(box, &QMessageBox::buttonClicked, this, [this, retry, items, account, bucket](QAbstractButton* b) {
            if (b == retry && m_operations)
                m_operations->start(new UploadJob(account, bucket, items, m_operations->planDir()));
        });
    }
    // "Retry Failed": a new delete of exactly the keys that failed.
    if (const auto* del = qobject_cast<const DeleteJob*>(job); del && !del->failedKeys().isEmpty()) {
        QPushButton* retry = box->addButton(tr("Retry Failed"), QMessageBox::ActionRole);
        QList<DeleteJob::Item> items;
        for (const QString& k : del->failedKeys())
            items.append({k, false});
        const Account account = del->account();
        const QString bucket = del->bucket();
        connect(box, &QMessageBox::buttonClicked, this, [this, retry, items, account, bucket](QAbstractButton* b) {
            if (b == retry && m_operations)
                m_operations->start(new DeleteJob(account, bucket, items, m_operations->planDir()));
        });
    }
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

// ---------------------------------------------------------------------------
// Object browser: navigation, filter, status bar
// ---------------------------------------------------------------------------
void MainWindow::connectBrowser_()
{
    BrowseController* nav = m_browser->navigation();
    connect(m_actBack, &QAction::triggered, nav, &BrowseController::back);
    connect(m_actForward, &QAction::triggered, nav, &BrowseController::forward);
    connect(m_actUp, &QAction::triggered, nav, &BrowseController::up);
    // F5 refreshes the side that has the focus.
    connect(m_actRefresh, &QAction::triggered, this, [this] {
        if (m_sidebar->view()->hasFocus())
            m_sidebar->refreshCurrent();
        else
            m_browser->refresh();
    });
    m_editMenu->addAction(m_browser->openAction());
    m_editMenu->addSeparator();
    for (QAction* a : {m_browser->cutAction(), m_browser->copyAction(), m_browser->pasteAction()})
        m_editMenu->addAction(a);
    m_editMenu->addSeparator();
    for (QAction* a : {m_browser->copyKeyAction(), m_browser->copyUriAction()})
        m_editMenu->addAction(a);
    m_editMenu->addSeparator();
    m_actNewFolder = m_editMenu->addAction(tr("&New Folder…"), this, &MainWindow::newFolder_);
    m_actNewFolder->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(m_newFolderBtn, &QToolButton::clicked, m_actNewFolder, &QAction::trigger);
    m_actUploadFiles = m_editMenu->addAction(tr("Upload &Files…"), this, &MainWindow::chooseUploadFiles_);
    m_actUploadFiles->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    m_actUploadFolder = m_editMenu->addAction(tr("Upload Fo&lder…"), this, &MainWindow::chooseUploadFolder_);
    auto* uploadMenu = new QMenu(m_uploadBtn);
    uploadMenu->addAction(m_actUploadFiles);
    uploadMenu->addAction(m_actUploadFolder);
    m_uploadBtn->setMenu(uploadMenu);
    m_uploadBtn->setPopupMode(QToolButton::MenuButtonPopup);
    connect(m_uploadBtn, &QToolButton::clicked, m_actUploadFiles, &QAction::trigger);
    connect(m_browser->deleteAction(), &QAction::triggered, this, &MainWindow::deleteSelection_);
    connect(m_browser->renameAction(), &QAction::triggered, this, &MainWindow::renameSelection_);
    connect(m_browser->editMetadataAction(), &QAction::triggered, this, &MainWindow::editMetadata_);
    connect(m_browser->changeClassAction(), &QAction::triggered, this, &MainWindow::changeStorageClass_);
    connect(m_browser->presignAction(), &QAction::triggered, this, &MainWindow::presign_);
    connect(m_deleteBtn, &QToolButton::clicked, m_browser->deleteAction(), &QAction::trigger);
    connect(m_browser->deleteAction(), &QAction::changed, this, [this] {
        m_deleteBtn->setEnabled(m_browser->deleteAction()->isEnabled());
        m_deleteBtn->setToolTip(m_browser->deleteAction()->toolTip());
    });
    m_editMenu->addAction(m_browser->renameAction());
    m_editMenu->addAction(m_browser->editMetadataAction());
    m_editMenu->addAction(m_browser->changeClassAction());
    m_editMenu->addAction(m_browser->deleteAction());
    m_editMenu->addSeparator();
    m_editMenu->addAction(m_browser->downloadAction());
    m_editMenu->addAction(m_browser->downloadToAction());
    m_editMenu->addAction(m_browser->presignAction());
    connect(m_browser->downloadAction(), &QAction::triggered, this, [this] { downloadSelection_(QString()); });
    connect(m_browser->downloadToAction(), &QAction::triggered, this, [this] {
        auto* dlg = new QFileDialog(this, tr("Download To"), m_settings->downloadDirectory());
        dlg->setFileMode(QFileDialog::Directory);
        dlg->setOption(QFileDialog::ShowDirsOnly);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QFileDialog::fileSelected, this, [this](const QString& dir) { downloadSelection_(dir); });
        dlg->open();
    });
    connect(m_downloadBtn, &QToolButton::clicked, m_browser->downloadAction(), &QAction::trigger);
    connect(m_browser->downloadAction(), &QAction::changed, this, [this] {
        m_downloadBtn->setEnabled(m_browser->downloadAction()->isEnabled());
        m_downloadBtn->setToolTip(m_browser->downloadAction()->toolTip());
    });
    connect(m_actEditPath, &QAction::triggered, m_browser, &ObjectBrowser::editPath);
    connect(m_backBtn, &QToolButton::clicked, m_actBack, &QAction::trigger);
    connect(m_forwardBtn, &QToolButton::clicked, m_actForward, &QAction::trigger);
    connect(m_upBtn, &QToolButton::clicked, m_actUp, &QAction::trigger);
    connect(m_refreshBtn, &QToolButton::clicked, m_actRefresh, &QAction::trigger);

    connect(m_filterField, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_browser->setFilterText(text);
        if (text.isEmpty() && m_browser->model() && !m_browser->model()->searchPrefix().isEmpty())
            m_browser->searchPrefix(QString());
    });
    // Enter asks the server: names starting with the text (all pages).
    connect(m_filterField, &QLineEdit::returnPressed, this,
            [this] { m_browser->searchPrefix(m_filterField->text()); });

    connect(nav, &BrowseController::historyChanged, this, &MainWindow::updateNavigation_);
    connect(m_browser, &ObjectBrowser::locationChanged, this, [this](const Location& here) {
        m_filterField->clear();   // the filter is per folder
        updateNavigation_();
        // What the bucket supports, before anything is done with it.
        if (m_capabilities && here.hasBucket())
            m_capabilities->bucketOpened(here.account, here.bucket);
    });
    if (m_capabilities) {
        connect(m_capabilities, &CapabilityController::changesDetected, this, [this](const QUuid& id) {
            const std::optional<Account> a = m_accounts->account(id);
            statusBar()->showMessage(
                tr("What “%1” supports has changed — see Tools → Server Capabilities.").arg(a ? a->name : QString()),
                10000);
        });
    }
    connect(m_browser, &ObjectBrowser::statusChanged, this, &MainWindow::updateStatus_);
    // The detail pane follows the selection (and is emptied by every
    // listing reset, which clears the selection).
    connect(m_browser->view()->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        const Location here = m_browser->navigation()->current();
        m_details->setSelection(here.account, here.bucket, m_browser->view()->selectionModel()->selectedRows());
    });
    connect(m_browser, &ObjectBrowser::locationChanged, m_details, &DetailPane::clear);
    updateNavigation_();
    updateStatus_();
}

void MainWindow::updateNavigation_()
{
    const BrowseController* nav = m_browser->navigation();
    const bool here = nav->current().hasBucket() && m_browser->model();
    m_actBack->setEnabled(nav->canGoBack());
    m_actForward->setEnabled(nav->canGoForward());
    m_actUp->setEnabled(nav->canGoUp());
    m_actRefresh->setEnabled(here || m_sessions);
    m_actEditPath->setEnabled(nav->current().isValid());
    m_actCapabilities->setEnabled(m_capabilities && nav->current().isValid());
    if (m_actIncompleteUploads) {
        OperationController::Check list{false, tr("Open a bucket first.")};
        if (m_operations && nav->current().hasBucket())
            list = m_operations->check(nav->current().account, nav->current().bucket, {s3::Feature::MultipartListing},
                                       false, false);
        m_actIncompleteUploads->setEnabled(list.allowed);
        m_actIncompleteUploads->setToolTip(list.allowed ? tr("Unfinished multipart uploads of this bucket") : list.reason);
    }
    updateMutationActions_();
    m_backBtn->setEnabled(nav->canGoBack());
    m_forwardBtn->setEnabled(nav->canGoForward());
    m_upBtn->setEnabled(nav->canGoUp());
    m_refreshBtn->setEnabled(here);
    m_filterField->setEnabled(here);
}

void MainWindow::showCapabilities_()
{
    const Location here = m_browser->navigation()->current();
    const std::optional<Account> account = m_accounts ? m_accounts->account(here.account) : std::nullopt;
    if (!m_capabilities || !account)
        return;
    auto* dlg = new CapabilitiesDialog(m_capabilities, account->id, account->name, here.bucket, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::showIncompleteUploads_()
{
    const Location here = m_browser->navigation()->current();
    if (!m_operations || !here.hasBucket())
        return;
    auto* dlg = new IncompleteUploadsDialog(m_operations, m_transfers, here.account, here.bucket, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::updateStatus_()
{
    const ObjectTableModel* model = m_browser->model();
    const QString text = m_browser->statusText();
    m_locationLabel->setText(text.isEmpty() ? tr("Not connected") : text);
    QColor dot = Theme::current().text3;
    QString state = tr("Offline");
    if (model && m_browser->navigation()->current().hasBucket()) {
        switch (model->state()) {
        case ObjectTableModel::State::Idle:
            break;
        case ObjectTableModel::State::Loading:
            dot = Theme::current().accent;
            state = tr("Loading…");
            break;
        case ObjectTableModel::State::Ready:
        case ObjectTableModel::State::Complete:
            dot = Theme::successColor();
            state = model->isStale() ? tr("Refreshing…") : tr("Online");
            break;
        case ObjectTableModel::State::Error:
            dot = Theme::dangerColor();
            state = tr("Error");
            break;
        }
    }
    m_connectionDot->setPixmap(statusDot(dot, devicePixelRatioF()));
    m_connectionLabel->setText(state);
    m_connectionLabel->setToolTip(model && model->state() == ObjectTableModel::State::Error ? model->errorText()
                                                                                           : QString());
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------
void MainWindow::buildMenuBar_()
{
    auto* mb = menuBar();

    auto* file = mb->addMenu(tr("&File"));
    m_actAddAccount = file->addAction(tr("Add &Account…"));
    m_actAddAccount->setEnabled(false);
    file->addSeparator();
    m_actQuit = file->addAction(tr("&Quit"), qApp, &QApplication::quit);
    setStandardShortcut(m_actQuit, QKeySequence::Quit);

    m_editMenu = mb->addMenu(tr("&Edit"));   // filled from the object browser
    m_editMenu->setToolTipsVisible(true);

    auto* view = mb->addMenu(tr("&View"));
    m_viewMenu = view;
    m_browseModeGroup = new QActionGroup(this);
    m_actFolders  = view->addAction(tr("&Folders"));
    m_actFlat     = view->addAction(tr("F&lat Listing"));
    m_actVersions = view->addAction(tr("&Versions"));
    const QList<QPair<QAction*, QString>> modes = {
        {m_actFolders,  QStringLiteral("folders")},
        {m_actFlat,     QStringLiteral("flat")},
        {m_actVersions, QStringLiteral("versions")},
    };
    for (const auto& entry : modes) {
        QAction* action = entry.first;
        const QString mode = entry.second;
        action->setCheckable(true);
        action->setData(mode);
        m_browseModeGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { m_settings->setBrowseMode(mode); });
    }

    m_actVersions->setEnabled(false);   // per bucket: updateMutationActions_()

    view->addSeparator();
    m_actBack = view->addAction(tr("&Back"));
    setStandardShortcut(m_actBack, QKeySequence::Back);
    m_actForward = view->addAction(tr("&Forward"));
    setStandardShortcut(m_actForward, QKeySequence::Forward);
    m_actUp = view->addAction(tr("&Up One Level"));
    m_actUp->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    m_actRefresh = view->addAction(tr("&Refresh"));
    setStandardShortcut(m_actRefresh, QKeySequence::Refresh);
    m_actEditPath = view->addAction(tr("&Go to Path…"));
    m_actEditPath->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    for (QAction* a : {m_actBack, m_actForward, m_actUp, m_actRefresh, m_actEditPath})
        a->setEnabled(false);

    view->addSeparator();
    auto* themeMenu = view->addMenu(tr("&Theme"));
    m_themeGroup = new QActionGroup(this);
    m_actThemeDark   = themeMenu->addAction(tr("&Dark"));
    m_actThemeLight  = themeMenu->addAction(tr("&Light"));
    m_actThemeSystem = themeMenu->addAction(tr("Follow &System"));
    const QList<QPair<QAction*, QString>> themes = {
        {m_actThemeDark,   QStringLiteral("Dark")},
        {m_actThemeLight,  QStringLiteral("Light")},
        {m_actThemeSystem, QStringLiteral("System")},
    };
    for (const auto& entry : themes) {
        QAction* action = entry.first;
        const QString name = entry.second;
        action->setCheckable(true);
        action->setData(name);
        m_themeGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, name] { m_settings->setThemeName(name); });
    }
    // The transfer-panel toggle is appended once the dock exists
    // (buildTransferDock_).
    view->addSeparator();

    auto* tools = mb->addMenu(tr("&Tools"));
    m_actCapabilities = tools->addAction(tr("Server &Capabilities…"), this, &MainWindow::showCapabilities_);
    m_actCapabilities->setEnabled(false);
    m_actIncompleteUploads = tools->addAction(tr("&Incomplete Uploads…"), this, &MainWindow::showIncompleteUploads_);
    m_actIncompleteUploads->setEnabled(false);
    tools->addSeparator();
    m_actSettings = tools->addAction(tr("&Settings…"),
                                     this, &MainWindow::showSettingsDialog_);
    setStandardShortcut(m_actSettings, QKeySequence::Preferences);

    auto* help = mb->addMenu(tr("&Help"));
    m_actAbout   = help->addAction(tr("&About QBucket…"), this, &MainWindow::showAbout_);
    m_actAboutQt = help->addAction(tr("About &Qt…"), qApp, &QApplication::aboutQt);
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void MainWindow::buildToolBar_()
{
    auto* tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));   // for saveState()
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setIconSize(QSize(16, 16));

    const auto spacer = [](int width = -1) {
        auto* w = new QWidget;
        if (width < 0)
            w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        else
            w->setFixedWidth(width);
        return w;
    };

    // Primary action. Every mutating action stays disabled until the S3
    // milestones wire it through the OperationController (PLAN.md §12.1).
    m_uploadBtn = new QToolButton;
    m_uploadBtn->setObjectName(QStringLiteral("tbPrimary"));
    m_uploadBtn->setText(tr("Upload"));
    m_uploadBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_uploadBtn->setToolTip(tr("Upload files or folders to the current location"));
    m_uploadBtn->setEnabled(false);
    tb->addWidget(m_uploadBtn);

    m_downloadBtn = makeIconButton(tr("Download the selection"));
    m_downloadBtn->setEnabled(false);
    tb->addWidget(m_downloadBtn);

    m_newFolderBtn = makeIconButton(tr("New folder"));
    m_newFolderBtn->setEnabled(false);
    tb->addWidget(m_newFolderBtn);

    m_deleteBtn = makeIconButton(tr("Delete the selection"));
    m_deleteBtn->setEnabled(false);
    tb->addWidget(m_deleteBtn);

    tb->addWidget(spacer(14));

    m_backBtn = makeIconButton(tr("Back"));
    m_backBtn->setEnabled(false);
    tb->addWidget(m_backBtn);

    m_forwardBtn = makeIconButton(tr("Forward"));
    m_forwardBtn->setEnabled(false);
    tb->addWidget(m_forwardBtn);

    m_upBtn = makeIconButton(tr("Up one level"));
    m_upBtn->setEnabled(false);
    tb->addWidget(m_upBtn);

    m_refreshBtn = makeIconButton(tr("Refresh"));
    m_refreshBtn->setEnabled(false);
    tb->addWidget(m_refreshBtn);

    tb->addWidget(spacer());

    m_filterField = new QLineEdit;
    m_filterField->setObjectName(QStringLiteral("filterField"));
    m_filterField->setPlaceholderText(tr("Filter this folder…"));
    m_filterField->setClearButtonEnabled(true);
    m_filterField->setMinimumWidth(220);
    m_filterField->setMaximumWidth(340);
    m_filterField->setEnabled(false);
    m_filterIcon = m_filterField->addAction(QIcon(), QLineEdit::LeadingPosition);
    tb->addWidget(m_filterField);

    tb->addWidget(spacer());

    // Segmented browse-mode control: Folders | Flat | Versions.
    auto* seg = new QWidget;
    seg->setObjectName(QStringLiteral("viewSeg"));
    auto* segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(2, 2, 2, 2);
    segLay->setSpacing(2);
    m_browseSeg = new QButtonGroup(this);
    m_browseSeg->setExclusive(true);
    const QList<QPair<QString, QString>> segments = {
        {QStringLiteral("folders"),  tr("Folders")},
        {QStringLiteral("flat"),     tr("Flat")},
        {QStringLiteral("versions"), tr("Versions")},
    };
    for (const auto& entry : segments) {
        const QString mode = entry.first;
        const QString& label = entry.second;
        auto* btn = new QToolButton;
        btn->setObjectName(QStringLiteral("segText"));
        btn->setText(label);
        btn->setCheckable(true);
        btn->setProperty("browseMode", mode);
        m_browseSeg->addButton(btn);
        segLay->addWidget(btn);
        connect(btn, &QToolButton::clicked, this,
                [this, mode] { m_settings->setBrowseMode(mode); });
        if (mode == QLatin1String("versions"))
            btn->setEnabled(false);   // per bucket: updateMutationActions_()
    }
    seg->setToolTip(tr("Folders: browse by prefix · Flat: every key below the "
                       "current prefix · Versions: all object versions"));
    tb->addWidget(seg);

    tb->addWidget(spacer(8));

    m_themeBtn = makeIconButton(tr("Toggle light / dark"));
    connect(m_themeBtn, &QToolButton::clicked, this, [this] {
        m_settings->setThemeName(m_theme->isDark() ? QStringLiteral("Light")
                                                   : QStringLiteral("Dark"));
    });
    tb->addWidget(m_themeBtn);

    m_settingsBtn = makeIconButton(tr("Settings"));
    connect(m_settingsBtn, &QToolButton::clicked, this, &MainWindow::showSettingsDialog_);
    tb->addWidget(m_settingsBtn);
}

// ---------------------------------------------------------------------------
// Central widget: sidebar | object area | detail pane
// ---------------------------------------------------------------------------
void MainWindow::buildCentralWidget_()
{
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setObjectName(QStringLiteral("mainSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(buildSidebar_());
    m_splitter->addWidget(buildObjectArea_());
    m_splitter->addWidget(buildDetailPane_());
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({kSidebarWidth, 800, kDetailWidth});
    setCentralWidget(m_splitter);

    const QByteArray splitState = QByteArray::fromBase64(
        m_settings->mainSplitterState().toLatin1());
    if (!splitState.isEmpty())
        m_splitter->restoreState(splitState);

    // Debounced: splitterMoved fires per mouse pixel, and persisting goes
    // through QSettings::sync() — a synchronous INI rewrite. Save once,
    // shortly after the drag settles (xyz-profiler lesson).
    auto* splitterSave = new QTimer(this);
    splitterSave->setSingleShot(true);
    splitterSave->setInterval(300);
    const auto persistSplitter = [this] {
        m_settings->setMainSplitterState(
            QString::fromLatin1(m_splitter->saveState().toBase64()));
    };
    connect(splitterSave, &QTimer::timeout, this, persistSplitter);
    connect(m_splitter, &QSplitter::splitterMoved, splitterSave,
            qOverload<>(&QTimer::start));
    // A drag followed by an immediate quit must not lose the new position.
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            [splitterSave, persistSplitter] {
        if (splitterSave->isActive()) {
            splitterSave->stop();
            persistSplitter();
        }
    });
}

QWidget* MainWindow::buildSidebar_()
{
    m_sidebar = new AccountSidebar(m_accounts, m_secrets, m_sessions);
    return m_sidebar;
}

QWidget* MainWindow::buildObjectArea_()
{
    m_browser = new ObjectBrowser(m_accounts, m_sessions);
    return m_browser;
}

QWidget* MainWindow::buildDetailPane_()
{
    m_details = new DetailPane(m_sessions);
    m_details->setControllers(m_operations, m_capabilities);
    connect(m_details, &DetailPane::editMetadataRequested, this, &MainWindow::editMetadata_);
    return m_details;
}

// ---------------------------------------------------------------------------
// Transfer dock
// ---------------------------------------------------------------------------
void MainWindow::buildTransferDock_()
{
    m_transferDock = new QDockWidget(tr("Transfers"), this);
    m_transferDock->setObjectName(QStringLiteral("transferDock"));   // saveState()
    m_transferDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_transferDock->setFeatures(QDockWidget::DockWidgetClosable);

    auto* tabs = new QTabWidget;
    m_dockTabs = tabs;
    tabs->setDocumentMode(true);
    if (m_operations) {
        m_transferModel = new TransferModel(m_operations, this);
        tabs->addTab(new TransferPanel(m_transferModel), tr("Transfers"));
        // A transfer starts: show where it runs.
        connect(m_transferModel, &TransferModel::transferStarted, this, [this] {
            m_transferDock->show();
            m_dockTabs->setCurrentIndex(0);
        });
    } else {
        tabs->addTab(makeEmptyState(tr("No transfers.")), tr("Transfers"));
    }
    tabs->addTab(new LogPanel(m_activity), tr("Log"));
    tabs->setMinimumHeight(120);
    m_transferDock->setWidget(tabs);
    addDockWidget(Qt::BottomDockWidgetArea, m_transferDock);

    // View → Transfers panel toggle (appended to the View menu).
    QAction* toggle = m_transferDock->toggleViewAction();
    toggle->setText(tr("&Transfers Panel"));
    m_viewMenu->addAction(toggle);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
void MainWindow::buildStatusBar_()
{
    auto* sb = statusBar();
    sb->setSizeGripEnabled(false);

    m_locationLabel = new QLabel(tr("Not connected"));
    sb->addWidget(m_locationLabel, 1);

    // A running operation: what, how far, and a way to stop it.
    m_jobLabel = new QLabel;
    m_jobLabel->hide();
    sb->addPermanentWidget(m_jobLabel);
    m_jobCancel = new QToolButton;
    m_jobCancel->setText(tr("Cancel"));
    m_jobCancel->hide();
    sb->addPermanentWidget(m_jobCancel);

    m_connectionDot = new QLabel;
    sb->addPermanentWidget(m_connectionDot);
    m_connectionLabel = new QLabel(tr("Offline"));
    sb->addPermanentWidget(m_connectionLabel);
}

// ---------------------------------------------------------------------------
// Settings & theme
// ---------------------------------------------------------------------------
void MainWindow::connectSettings_()
{
    connect(m_settings, &SettingsController::browseModeChanged,
            this, &MainWindow::applyBrowseMode_);
    connect(m_settings, &SettingsController::themeNameChanged,
            this, &MainWindow::applyThemeChoice_);
    connect(m_theme, &ThemeManager::themeChanged, this, [this] {
        refreshIcons_();
        if (m_browser) m_browser->view()->viewport()->update();
        updateStatus_();
        if (m_sidebar) m_sidebar->refreshIcons();
    });
}

void MainWindow::applyBrowseMode_()
{
    const QString mode = m_settings->browseMode();
    for (QAction* action : m_browseModeGroup->actions())
        action->setChecked(action->data().toString() == mode);
    for (QAbstractButton* btn : m_browseSeg->buttons())
        btn->setChecked(btn->property("browseMode").toString() == mode);
    if (m_browser) {
        m_browser->setMode(mode == QLatin1String("flat")       ? ObjectTableModel::Mode::Flat
                           : mode == QLatin1String("versions") ? ObjectTableModel::Mode::Versions
                                                               : ObjectTableModel::Mode::Folders);
    }
}

void MainWindow::applyThemeChoice_()
{
    const QString name = m_settings->themeName();
    for (QAction* action : m_themeGroup->actions())
        action->setChecked(action->data().toString() == name);
}

void MainWindow::refreshIcons_()
{
    const Palette& p = Theme::current();
    m_uploadBtn->setIcon(IconFactory::icon(QStringLiteral("upload"), p.accentFg, 16));
    m_downloadBtn->setIcon(IconFactory::icon(QStringLiteral("download"), p.text2, 16));
    m_newFolderBtn->setIcon(IconFactory::icon(QStringLiteral("folderPlus"), p.text2, 16));
    m_deleteBtn->setIcon(IconFactory::icon(QStringLiteral("trash"), p.text2, 16));
    m_backBtn->setIcon(IconFactory::icon(QStringLiteral("arrowLeft"), p.text2, 16));
    m_forwardBtn->setIcon(IconFactory::icon(QStringLiteral("arrowRight"), p.text2, 16));
    m_upBtn->setIcon(IconFactory::icon(QStringLiteral("arrowUp"), p.text2, 16));
    m_refreshBtn->setIcon(IconFactory::icon(QStringLiteral("refresh"), p.text2, 16));
    m_filterIcon->setIcon(IconFactory::icon(QStringLiteral("filter"), p.text3, 16));
    m_settingsBtn->setIcon(IconFactory::icon(QStringLiteral("settings"), p.text2, 16));
    m_themeBtn->setIcon(IconFactory::icon(
        Theme::isDark() ? QStringLiteral("sun") : QStringLiteral("moon"), p.text2, 16));

    m_connectionDot->setPixmap(statusDot(p.text3, devicePixelRatioF()));
}

void MainWindow::showSettingsDialog_()
{
    SettingsDialog dlg(m_settings, this);
    dlg.exec();
}

void MainWindow::showAddAccountDialog_()
{
    // Non-modal-safe: open(), never exec() — the dialog does asynchronous
    // keychain work and connection tests.
    auto* dlg = new AccountDialog(m_accounts, m_secrets, std::nullopt, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &AccountDialog::accountSaved, this, [this](const Account& a, bool sessionOnly) {
        statusBar()->showMessage(sessionOnly
            ? tr("Account “%1” saved — its secret is kept for this session only.").arg(a.name)
            : tr("Account “%1” saved.").arg(a.name), 8000);
    });
    dlg->open();
}

void MainWindow::showAbout_()
{
    QMessageBox::about(this, tr("About QBucket"),
        tr("<h3>QBucket %1</h3>"
           "<p>A native browser for S3 and S3-compatible object storage.</p>"
           "<p>Built with Qt %2 Widgets. Licensed under the MIT license.</p>")
            .arg(QStringLiteral(QBUCKET_VERSION_STRING),
                 QString::fromLatin1(qVersion())));
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------
void MainWindow::restoreWindowState_()
{
    const QByteArray geometry = QByteArray::fromBase64(
        m_settings->windowGeometry().toLatin1());
    if (geometry.isEmpty() || !restoreGeometry(geometry))
        resize(1400, 880);

    const QByteArray state = QByteArray::fromBase64(
        m_settings->windowState().toLatin1());
    if (!state.isEmpty())
        restoreState(state);
}

void MainWindow::saveWindowState_()
{
    // One-shot on close — not a per-event path, so the synchronous settings
    // write is fine here.
    m_settings->setWindowGeometry(QString::fromLatin1(saveGeometry().toBase64()));
    m_settings->setWindowState(QString::fromLatin1(saveState().toBase64()));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Let the account sessions stop their worker threads without blocking
    // (PLAN.md §3, orderly shutdown), then close for real.
    if (m_sessions && !m_sessions->isShutDown()) {
        event->ignore();
        saveWindowState_();
        // Transfers stop but keep everything to continue next time; the
        // other jobs are cancelled.
        if (m_operations) {
            for (Job* job : m_operations->jobs())
                job->suspend();
        }
        connect(m_sessions, &AccountSessions::allStopped, this, &QWidget::close, Qt::QueuedConnection);
        // A worker that doesn't end in time is left to ~AccountSessions.
        QTimer::singleShot(3000, this, &QWidget::close);
        m_sessions->shutdown();
        return;
    }
    if (!m_sessions)
        saveWindowState_();
    if (m_transfers) {
        // What the suspended transfers wrote must be on disk.
        s3::transfer::flushResumeWrites(2000);
        m_transfers->jobs()->waitForWrites(2000);
    }
    QMainWindow::closeEvent(event);
}

} // namespace qbucket
