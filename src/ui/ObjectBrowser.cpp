#include "ui/ObjectBrowser.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/BrowseController.h"
#include "ui/ObjectRowDelegate.h"
#include "ui/PathBar.h"

#include <QAction>
#include <QClipboard>
#include <QDropEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace qbucket {

namespace {

constexpr int kMessagePage = 0;
constexpr int kListPage = 1;

} // namespace

ObjectBrowser::ObjectBrowser(AccountStore* accounts, AccountSessions* sessions, QWidget* parent)
    : QWidget(parent),
      m_accounts(accounts),
      m_sessions(sessions),
      m_nav(new BrowseController(this))
{
    setMinimumWidth(360);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_pathBar = new PathBar;
    lay->addWidget(m_pathBar);

    m_banner = new QLabel;
    m_banner->setObjectName(QStringLiteral("bannerInfo"));
    m_banner->setWordWrap(true);
    m_banner->hide();
    lay->addWidget(m_banner);

    m_stack = new QStackedWidget;
    auto* messagePage = new QWidget;
    // Holds the keyboard focus while the list is replaced by a message
    // (loading a folder), so Backspace & co. keep working.
    messagePage->setFocusPolicy(Qt::StrongFocus);
    m_messagePage = messagePage;
    auto* messageLay = new QVBoxLayout(messagePage);
    messageLay->addStretch(1);
    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("emptyState"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    messageLay->addWidget(m_message);
    m_retry = new QPushButton(tr("Try Again"));
    m_retry->hide();
    messageLay->addWidget(m_retry, 0, Qt::AlignHCenter);
    messageLay->addStretch(2);
    m_stack->addWidget(messagePage);

    // A QTableView, not a QTreeView: the tree keeps an item per row and lays
    // all of them out again after every inserted page — 47 ms per page at
    // 1M rows against 9 ms (bench_listing). Column widths are fixed +
    // Interactive: ResizeToContents is O(rows) per resize (xyz-profiler).
    m_view = new QTableView;
    m_view->setObjectName(QStringLiteral("objectView"));
    m_view->setShowGrid(false);
    m_view->setWordWrap(false);
    m_view->setCornerButtonEnabled(false);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 10);
    m_view->horizontalHeader()->setHighlightSections(false);
    m_view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_delegate = new ObjectRowDelegate(m_view);
    m_view->setItemDelegate(m_delegate);
    // Files and folders from the desktop's file manager upload here.
    m_view->setAcceptDrops(true);
    m_view->viewport()->setAcceptDrops(true);
    // Rows drag out as objects (inside QBucket); files and objects drop in.
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    m_view->setDragEnabled(true);
    m_view->setDefaultDropAction(Qt::CopyAction);
    m_view->setDropIndicatorShown(false);
    m_stack->addWidget(m_view);
    lay->addWidget(m_stack, 1);
    buildActions_();

    if (!m_sessions) {
        m_pathBar->clearLocation(tr("No location"));
        showMessage_(tr("Add an account to start browsing your buckets."), false);
        return;
    }

    m_model = new ObjectTableModel(m_sessions, this);
    m_view->setModel(m_model);
    auto* header = m_view->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(ObjectTableModel::NameColumn, QHeaderView::Stretch);
    for (int col = ObjectTableModel::SizeColumn; col < ObjectTableModel::ColumnCount; ++col)
        header->setSectionResizeMode(col, QHeaderView::Interactive);
    header->resizeSection(ObjectTableModel::SizeColumn, 110);
    header->resizeSection(ObjectTableModel::ModifiedColumn, 170);
    header->resizeSection(ObjectTableModel::StorageClassColumn, 130);
    // Server order until a column is clicked; a third click returns to it.
    header->setSortIndicatorClearable(true);
    header->setSortIndicator(-1, Qt::AscendingOrder);
    m_view->setSortingEnabled(true);

    connect(m_nav, &BrowseController::locationChanged, this, &ObjectBrowser::onLocationChanged_);
    connect(m_accounts, &AccountStore::accountsChanged, this, &ObjectBrowser::onAccountsChanged_);
    connect(m_pathBar, &PathBar::crumbActivated, this, [this](const QString& bucket, const QString& prefix) {
        m_nav->navigate({m_nav->current().account, bucket, prefix});
    });
    connect(m_pathBar, &PathBar::pathEntered, this, [this](const QString& bucket, const QString& prefix) {
        m_nav->navigate({m_nav->current().account, bucket, prefix});
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        if (m_model->state() == ObjectTableModel::State::Error)
            refresh();
        else
            m_model->loadMore();   // "Load More" with a filter that hides everything
    });
    connect(m_view, &QAbstractItemView::activated, this, &ObjectBrowser::onActivated_);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &ObjectBrowser::maybePrefetch_);
    m_view->viewport()->installEventFilter(this);   // a taller window may need the next page

    const auto changed = [this] {
        updatePage_();
        emit statusChanged();
    };
    connect(m_model, &ObjectTableModel::stateChanged, this, changed);
    connect(m_model, &ObjectTableModel::staleChanged, this, changed);
    connect(m_model, &ObjectTableModel::pageLoaded, this, [this, changed] {
        changed();
        maybePrefetch_();
    });
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &ObjectBrowser::rememberCurrent_);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this, changed] {
        restoreCurrent_();
        changed();
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActions_();
        emit statusChanged();
    });
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &ObjectBrowser::showContextMenu_);
    connect(m_actOpen, &QAction::triggered, this, [this] {
        const QModelIndexList rows = selectedRows();
        if (rows.size() == 1)
            onActivated_(rows.first());
    });
    connect(m_actCopyKey, &QAction::triggered, this, [this] { copySelection_(false); });
    connect(m_actCopyUri, &QAction::triggered, this, [this] { copySelection_(true); });

    m_pathBar->clearLocation(tr("No location"));
    updatePage_();
}

void ObjectBrowser::openBucket(const QUuid& account, const QString& bucket)
{
    m_nav->navigate({account, bucket, QString()});
}

void ObjectBrowser::setMode(ObjectTableModel::Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    const Location here = m_nav->current();
    if (m_model && here.hasBucket())
        m_model->setLocation(here.account, here.bucket, here.prefix, m_mode);
}

void ObjectBrowser::refresh()
{
    if (m_model && m_nav->current().hasBucket())
        m_model->reload();
}

void ObjectBrowser::setFilterText(const QString& text)
{
    if (m_model)
        m_model->setFilterText(text);
}

void ObjectBrowser::searchPrefix(const QString& text)
{
    if (m_model && m_nav->current().hasBucket())
        m_model->setSearchPrefix(text);
}

void ObjectBrowser::editPath()
{
    m_pathBar->editPath();
}

void ObjectBrowser::onLocationChanged_(const Location& location)
{
    const std::optional<Account> account = location.isValid() ? m_accounts->account(location.account)
                                                              : std::nullopt;
    if (!account) {
        m_model->clear();
        m_pathBar->clearLocation(tr("No location"));
    } else {
        m_pathBar->setLocation(account->name, int(account->environment), location);
        if (location.hasBucket())
            m_model->setLocation(location.account, location.bucket, location.prefix, m_mode);
        else
            m_model->clear();
    }
    m_view->scrollToTop();
    updatePage_();
    emit locationChanged(location);
    emit statusChanged();
}

void ObjectBrowser::onAccountsChanged_()
{
    const Location here = m_nav->current();
    if (!here.isValid()) {
        updatePage_();   // "add an account" / "select a bucket"
        return;
    }
    const std::optional<Account> account = m_accounts->account(here.account);
    if (!account) {
        m_nav->forgetAccount(here.account);
        return;
    }
    m_pathBar->setLocation(account->name, int(account->environment), here);   // name or environment edited
}

void ObjectBrowser::updatePage_()
{
    if (!m_model)
        return;
    using State = ObjectTableModel::State;
    const State state = m_model->state();
    const int rows = m_model->rowCount();
    const QString since = QLocale().toString(m_model->staleSince().toLocalTime().time(), QLocale::ShortFormat);

    QString banner;
    bool bannerError = false;
    if (state == State::Error && m_model->isStale()) {
        banner = tr("Could not refresh — showing the listing from %1: %2").arg(since, m_model->errorText());
        bannerError = true;
    } else if (state == State::Error && rows > 0) {
        banner = tr("The listing is incomplete: %1").arg(m_model->errorText());
        bannerError = true;
    } else if (m_model->isStale()) {
        banner = tr("Showing the listing from %1 — refreshing…").arg(since);
    }
    const QString bannerName = bannerError ? QStringLiteral("bannerError") : QStringLiteral("bannerInfo");
    if (m_banner->objectName() != bannerName) {
        m_banner->setObjectName(bannerName);
        m_banner->style()->unpolish(m_banner);
        m_banner->style()->polish(m_banner);
    }
    m_banner->setText(banner);
    m_banner->setVisible(!banner.isEmpty());

    if (!m_nav->current().hasBucket()) {
        if (m_nav->current().isValid())
            showMessage_(tr("Select a bucket of this account in the sidebar."), false);
        else if (m_accounts->accounts().isEmpty())
            showMessage_(tr("Add an account to start browsing your buckets."), false);
        else
            showMessage_(tr("Select a bucket in the sidebar."), false);
        return;
    }
    if (rows > 0) {
        const bool focusBack = m_messagePage->hasFocus();
        m_stack->setCurrentIndex(kListPage);
        if (focusBack)
            m_view->setFocus();
        return;
    }
    switch (state) {
    case State::Idle:
    case State::Loading:
        showMessage_(tr("Loading…"), false);
        return;
    case State::Error:
        m_retry->setText(tr("Try Again"));
        showMessage_(m_model->errorText(), true);
        return;
    case State::Ready:
    case State::Complete:
        break;
    }
    if (!m_model->filterText().isEmpty()) {
        const bool more = state == State::Ready;
        m_retry->setText(tr("Load More"));
        showMessage_(more ? tr("Nothing loaded so far matches “%1”; more entries can be loaded.")
                                .arg(m_model->filterText())
                          : tr("Nothing here matches “%1”.").arg(m_model->filterText()),
                     more);
    } else if (!m_model->searchPrefix().isEmpty()) {
        showMessage_(tr("Nothing here starts with “%1”.").arg(m_model->searchPrefix()), false);
    } else if (state == State::Ready) {
        showMessage_(tr("Loading…"), false);   // an empty page; the model lists on
    } else {
        showMessage_(tr("This folder is empty."), false);
    }
}

void ObjectBrowser::showMessage_(const QString& text, bool retry)
{
    m_message->setText(text);
    m_retry->setVisible(retry);
    const bool keepFocus = m_view->hasFocus();
    m_stack->setCurrentIndex(kMessagePage);
    if (keepFocus)
        m_messagePage->setFocus();
}

void ObjectBrowser::onActivated_(const QModelIndex& index)
{
    if (!index.data(ObjectTableModel::IsFolderRole).toBool())
        return;
    const Location here = m_nav->current();
    m_nav->navigate({here.account, here.bucket, index.data(ObjectTableModel::KeyRole).toString()});
}

void ObjectBrowser::maybePrefetch_()
{
    // The browser decides about paging (ObjectTableModel::loadMore): ask for
    // the next page before the end is reached (PLAN.md §13.1), and to fill
    // a viewport the rows don't. Measured on the last row, not the scroll
    // bar: right after a page arrives the view hasn't laid it out yet
    // (visualRect does), and the bar would still read "at the end".
    if (!m_model || !m_model->canLoadMore())
        return;
    const int rows = m_model->rowCount();
    const int height = m_view->viewport()->height();
    const QRect last = rows > 0 ? m_view->visualRect(m_model->index(rows - 1, 0)) : QRect();
    if (rows == 0 || (last.isValid() && last.top() < 3 * height))
        m_model->loadMore();
}

bool ObjectBrowser::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_view->viewport()) {
        if (event->type() == QEvent::Resize)
            QMetaObject::invokeMethod(this, &ObjectBrowser::maybePrefetch_, Qt::QueuedConnection);
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::DragLeave:
        case QEvent::Drop:
            return handleDrop_(event);
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ObjectBrowser::setUploadsAllowed(bool allowed)
{
    m_uploadsAllowed = allowed;
}

QStringList ObjectBrowser::localPaths(const QMimeData* mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile())
            return {};   // a web link, a remote share URL: not ours to fetch
        paths.append(url.toLocalFile());
    }
    return paths;
}

QString ObjectBrowser::dropPrefixAt_(const QPoint& pos, int* row) const
{
    *row = -1;
    const Location here = m_nav->current();
    if (!here.hasBucket())
        return {};
    const QModelIndex at = m_view->indexAt(pos);
    if (at.isValid() && at.data(ObjectTableModel::IsFolderRole).toBool()) {
        *row = at.row();
        return at.data(ObjectTableModel::KeyRole).toString();
    }
    return here.prefix;
}

void ObjectBrowser::setDropRow_(int row)
{
    if (m_delegate->dropRow() == row)
        return;
    m_delegate->setDropRow(row);
    m_view->viewport()->update();
}

bool ObjectBrowser::handleDrop_(QEvent* event)
{
    if (event->type() == QEvent::DragLeave) {
        setDropRow_(-1);
        return true;
    }
    auto* drop = static_cast<QDropEvent*>(event);
    if (const std::optional<ObjectSelection> sel = ObjectSelection::fromMimeData(drop->mimeData())) {
        int row = -1;
        const QString prefix = dropPrefixAt_(drop->position().toPoint(), &row);
        // Not onto one of the dragged rows themselves.
        bool onSelf = false;
        if (row >= 0) {
            const QString key = m_model->index(row, 0).data(ObjectTableModel::KeyRole).toString();
            for (const ObjectSelection::Item& i : sel->items)
                onSelf = onSelf || (sel->bucket == m_nav->current().bucket && i.key == key);
        }
        if (onSelf || !acceptsObjects_(*sel, prefix)) {
            drop->ignore();
            setDropRow_(-1);
            return true;
        }
        const bool move = drop->modifiers() & Qt::ShiftModifier;
        drop->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            setDropRow_(-1);
            emit objectsDropped(*sel, prefix, move);
            return true;
        }
        setDropRow_(row);
        return true;
    }
    const QStringList paths = localPaths(drop->mimeData());
    if (!m_uploadsAllowed || paths.isEmpty() || !m_nav->current().hasBucket()) {
        drop->ignore();
        setDropRow_(-1);
        return true;
    }
    int row = -1;
    const QString prefix = dropPrefixAt_(drop->position().toPoint(), &row);
    if (event->type() == QEvent::Drop) {
        setDropRow_(-1);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        emit filesDropped(paths, prefix);
        return true;
    }
    setDropRow_(row);
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    return true;
}

void ObjectBrowser::rememberCurrent_()
{
    const QModelIndex current = m_view->currentIndex();
    m_currentKey = current.isValid() ? current.data(ObjectTableModel::KeyRole).toString() : QString();
}

void ObjectBrowser::restoreCurrent_()
{
    if (m_currentKey.isEmpty())
        return;
    const int row = m_model->rowOfKey(m_currentKey);
    if (row >= 0) {
        const QModelIndex index = m_model->index(row, 0);
        m_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect
                                                             | QItemSelectionModel::Rows);
    }
}

void ObjectBrowser::buildActions_()
{
    const auto make = [this](const QString& text, const QKeySequence& key) {
        auto* a = new QAction(text, this);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetShortcut);
        a->setEnabled(false);
        m_view->addAction(a);
        return a;
    };
    m_actOpen = make(tr("&Open"), QKeySequence());
    // Like a file manager: Ctrl+C / X / V copy, cut and paste objects.
    m_actCopy = make(tr("&Copy"), QKeySequence::Copy);
    m_actCut = make(tr("Cut"), QKeySequence::Cut);   // no mnemonic: C, u and t are taken (Copy, URI, To)
    m_actPaste = make(tr("&Paste"), QKeySequence::Paste);
    connect(m_actCopy, &QAction::triggered, this, [this] { copyObjects_(ObjectSelection::Intent::Copy); });
    connect(m_actCut, &QAction::triggered, this, [this] { copyObjects_(ObjectSelection::Intent::Cut); });
    connect(m_actPaste, &QAction::triggered, this, &ObjectBrowser::paste_);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ObjectBrowser::updateActions_);
    m_actCopyKey = make(tr("Copy &Key"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_actCopyUri = make(tr("Copy S3 &URI"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    // Mutations go through the OperationController (PLAN.md §12.1); until
    // it handles them, the actions say why they can't be used.
    m_actRename = make(tr("&Rename…"), QKeySequence(Qt::Key_F2));
    m_actEditMetadata = make(tr("Edit &Metadata…"), QKeySequence());
    m_actChangeClass = make(tr("Change Storage Cl&ass…"), QKeySequence());
    m_actPresign = make(tr("Pre&signed Link…"), QKeySequence());
    m_actDelete = make(tr("&Delete…"), QKeySequence::Delete);
    m_actDownload = make(tr("Do&wnload"), QKeySequence(Qt::CTRL | Qt::Key_D));
    m_actDownloadTo = make(tr("Download &To…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));

    // Backspace goes up, like a file manager — wherever the focus is in
    // the browser (a line edit keeps its own Backspace).
    auto* up = new QAction(this);
    up->setShortcut(QKeySequence(Qt::Key_Backspace));
    up->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(up, &QAction::triggered, m_nav, &BrowseController::up);
    addAction(up);
}

void ObjectBrowser::updateActions_()
{
    const QModelIndexList rows = selectedRows();
    m_actOpen->setEnabled(rows.size() == 1 && rows.first().data(ObjectTableModel::IsFolderRole).toBool());
    m_actCopyKey->setEnabled(!rows.isEmpty());
    m_actCopyUri->setEnabled(!rows.isEmpty());
    m_actCopy->setEnabled(!rows.isEmpty());
    m_actCut->setEnabled(!rows.isEmpty() && m_mutationsAllowed);
    m_actCut->setToolTip(m_mutationsAllowed ? tr("Cut the selection: Paste moves it") : m_mutationsReason);
    QString why;
    const bool canPaste = pasteable_(&why).has_value();
    m_actPaste->setEnabled(canPaste);
    m_actPaste->setToolTip(canPaste ? tr("Paste here (copied objects are copied, cut ones moved)") : why);
    m_actDelete->setEnabled(!rows.isEmpty() && m_mutationsAllowed);
    m_actDelete->setToolTip(m_mutationsAllowed ? tr("Delete the selection") : m_mutationsReason);
    m_actRename->setEnabled(rows.size() == 1 && m_mutationsAllowed);
    m_actRename->setToolTip(m_mutationsAllowed ? tr("Rename the selected object or folder") : m_mutationsReason);
    m_actEditMetadata->setEnabled(!rows.isEmpty() && m_mutationsAllowed);
    m_actEditMetadata->setToolTip(m_mutationsAllowed ? tr("Change headers and user metadata of the selection")
                                                     : m_mutationsReason);
    m_actChangeClass->setEnabled(!rows.isEmpty() && m_mutationsAllowed && m_classesAllowed);
    m_actChangeClass->setToolTip(!m_mutationsAllowed ? m_mutationsReason
                                 : !m_classesAllowed ? m_classesReason
                                                     : tr("Move the selection to another storage class"));
    bool anyObject = false;
    for (const QModelIndex& r : rows)
        anyObject = anyObject || !r.data(ObjectTableModel::IsFolderRole).toBool();
    m_actPresign->setEnabled(anyObject && m_downloadsAllowed);
    m_actPresign->setToolTip(m_downloadsAllowed ? tr("Links to the selected objects that work without credentials")
                                                : m_downloadsReason);
    for (QAction* a : {m_actDownload, m_actDownloadTo}) {
        a->setEnabled(!rows.isEmpty() && m_downloadsAllowed);
        a->setToolTip(m_downloadsAllowed ? tr("Download the selection") : m_downloadsReason);
    }
}

void ObjectBrowser::setDownloadsAllowed(bool allowed, const QString& reason)
{
    m_downloadsAllowed = allowed;
    m_downloadsReason = reason;
    updateActions_();
}

void ObjectBrowser::setStorageClassesAllowed(bool allowed, const QString& reason)
{
    m_classesAllowed = allowed;
    m_classesReason = reason;
    updateActions_();
}

void ObjectBrowser::setMutationsAllowed(bool allowed, const QString& reason)
{
    m_mutationsAllowed = allowed;
    m_mutationsReason = reason;
    updateActions_();
}

QModelIndexList ObjectBrowser::selectedRows() const
{
    if (!m_view->selectionModel())
        return {};
    QModelIndexList rows = m_view->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    return rows;
}

void ObjectBrowser::copySelection_(bool asUri)
{
    const QString bucket = m_nav->current().bucket;
    QStringList lines;
    for (const QModelIndex& row : selectedRows()) {
        const QString key = row.data(ObjectTableModel::KeyRole).toString();
        lines.append(asUri ? QStringLiteral("s3://%1/%2").arg(bucket, key) : key);
    }
    if (lines.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(lines.join(u'\n'));
}

ObjectSelection ObjectBrowser::selection(ObjectSelection::Intent intent) const
{
    ObjectSelection sel;
    const Location here = m_nav->current();
    sel.account = here.account;
    sel.bucket = here.bucket;
    sel.base = m_model->prefix();
    sel.intent = intent;
    for (const QModelIndex& row : selectedRows())
        sel.items.append({row.data(ObjectTableModel::KeyRole).toString(), row.data(ObjectTableModel::IsFolderRole).toBool()});
    return sel;
}

void ObjectBrowser::copyObjects_(ObjectSelection::Intent intent)
{
    const ObjectSelection sel = selection(intent);
    if (sel.isEmpty() || !m_nav->current().hasBucket())
        return;
    QGuiApplication::clipboard()->setMimeData(sel.toMimeData());
}

std::optional<ObjectSelection> ObjectBrowser::pasteable_(QString* why) const
{
    std::optional<ObjectSelection> sel = ObjectSelection::fromMimeData(QGuiApplication::clipboard()->mimeData());
    const Location here = m_nav->current();
    if (!sel || !here.hasBucket())
        return std::nullopt;
    if (sel->account != here.account) {
        *why = tr("Copying between accounts comes in a later version.");
        return std::nullopt;
    }
    if (!m_uploadsAllowed) {
        *why = m_mutationsReason.isEmpty() ? tr("This account can't write here.") : m_mutationsReason;
        return std::nullopt;
    }
    return sel;
}

void ObjectBrowser::paste_()
{
    QString why;
    if (const std::optional<ObjectSelection> sel = pasteable_(&why))
        emit pasteRequested(*sel, m_nav->current().prefix);
}

bool ObjectBrowser::acceptsObjects_(const ObjectSelection& sel, const QString& prefix) const
{
    const Location here = m_nav->current();
    if (!m_uploadsAllowed || sel.account != here.account || !here.hasBucket())
        return false;
    if (sel.bucket != here.bucket)
        return true;
    if (prefix == sel.base)
        return false;   // where they are already: a slip of the mouse
    for (const ObjectSelection::Item& i : sel.items) {
        if (i.folder && prefix.startsWith(i.key))
            return false;   // into itself
    }
    return true;
}

void ObjectBrowser::showContextMenu_(const QPoint& pos)
{
    if (!m_model || !m_nav->current().hasBucket())
        return;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    if (m_actOpen->isEnabled())
        menu.addAction(m_actOpen);
    menu.addAction(m_actCopy);
    menu.addAction(m_actCut);
    menu.addAction(m_actPaste);
    menu.addSeparator();
    menu.addAction(m_actCopyKey);
    menu.addAction(m_actCopyUri);
    menu.addSeparator();
    menu.addAction(m_actDownload);
    menu.addAction(m_actDownloadTo);
    menu.addAction(m_actPresign);
    menu.addSeparator();
    menu.addAction(m_actRename);
    menu.addAction(m_actEditMetadata);
    menu.addAction(m_actChangeClass);
    menu.addAction(m_actDelete);
    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, &ObjectBrowser::refresh);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

QString ObjectBrowser::statusText() const
{
    if (!m_model || !m_nav->current().hasBucket() || m_model->state() == ObjectTableModel::State::Loading)
        return {};
    const QLocale locale;
    QString text = tr("%n object(s)", nullptr, int(m_model->loadedObjects()));
    if (m_model->loadedFolders() > 0)
        text = tr("%1, %n folder(s)", nullptr, int(m_model->loadedFolders())).arg(text);
    text = tr("%1 · %2").arg(text, locale.formattedDataSize(m_model->loadedBytes()));
    if (m_model->state() == ObjectTableModel::State::Ready)
        text = tr("%1 loaded so far").arg(text);
    const qsizetype selected = m_view->selectionModel() ? m_view->selectionModel()->selectedRows().size() : 0;
    if (selected > 0)
        text = tr("%1 · %n selected", nullptr, int(selected)).arg(text);
    return text;
}

} // namespace qbucket
