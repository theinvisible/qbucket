#include "ui/OpenFilesDialog.h"

#include "accounts/AccountStore.h"
#include "controllers/OpenFileService.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QString stateText(const OpenFileService::Entry& e)
{
    using S = OpenFileService::State;
    switch (e.state) {
    case S::Downloading: return OpenFilesDialog::tr("Downloading…");
    case S::Open: return e.readOnly ? OpenFilesDialog::tr("Open (read-only)") : OpenFilesDialog::tr("Open");
    case S::Changed:
        return e.error.isEmpty() ? OpenFilesDialog::tr("Changed — not uploaded")
                                 : OpenFilesDialog::tr("Changed — not uploaded: %1").arg(e.error);
    case S::Uploading: return OpenFilesDialog::tr("Uploading…");
    case S::Failed: return OpenFilesDialog::tr("Failed: %1").arg(e.error);
    }
    return {};
}

} // namespace

OpenFilesDialog::OpenFilesDialog(OpenFileService* files, AccountStore* accounts, QWidget* parent)
    : QDialog(parent),
      m_files(files),
      m_accounts(accounts)
{
    setWindowTitle(tr("Open Files"));
    setMinimumSize(780, 360);
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Objects opened in another application. Changes are uploaded only when you say so."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    m_table = new QTableWidget(0, 3);
    m_table->setObjectName(QStringLiteral("openFiles"));
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Location"), tr("State")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 200);
    m_table->setColumnWidth(1, 300);
    lay->addWidget(m_table, 1);
    m_empty = new QLabel(tr("No objects are open."));
    m_empty->setObjectName(QStringLiteral("emptyState"));
    lay->addWidget(m_empty);

    auto* row = new QHBoxLayout;
    m_upload = new QPushButton(tr("Upload Changes"));
    m_upload->setObjectName(QStringLiteral("primary"));
    m_ignore = new QPushButton(tr("Ignore Changes"));
    m_again = new QPushButton(tr("Open Again"));
    m_folder = new QPushButton(tr("Show Folder"));
    m_discard = new QPushButton(tr("Close File"));
    m_discard->setToolTip(tr("Stop watching it and delete the local copy"));
    for (QPushButton* b : {m_upload, m_ignore, m_again, m_folder, m_discard})
        row->addWidget(b);
    row->addStretch(1);
    lay->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(m_upload, &QPushButton::clicked, this, [this] { m_files->upload(current_()); });
    connect(m_ignore, &QPushButton::clicked, this, [this] { m_files->ignoreChange(current_()); });
    connect(m_again, &QPushButton::clicked, this, [this] { m_files->openAgain(current_()); });
    connect(m_folder, &QPushButton::clicked, this, [this] {
        if (const OpenFileService::Entry* e = m_files->entry(current_()))
            emit showFolderRequested(e->path);
    });
    connect(m_discard, &QPushButton::clicked, this, [this] {
        const QUuid id = current_();
        const OpenFileService::Entry* e = m_files->entry(id);
        if (!e)
            return;
        if (e->state != OpenFileService::State::Changed || e->readOnly) {
            m_files->discard(id);
            return;
        }
        // The edits exist only in this copy: never deleted unasked.
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Discard Changes"),
                                    tr("Delete the changed copy of “%1”? Its changes were not uploaded and are lost.")
                                        .arg(e->key.section(u'/', -1)),
                                    QMessageBox::Cancel, this);
        QPushButton* discard = box->addButton(tr("Discard Changes"), QMessageBox::DestructiveRole);
        box->setDefaultButton(QMessageBox::Cancel);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QMessageBox::finished, this, [this, box, discard, id] {
            if (box->clickedButton() == discard)
                m_files->discard(id);
        });
        box->open();
    });
    connect(m_files, &OpenFileService::entriesChanged, this, &OpenFilesDialog::reload_);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &OpenFilesDialog::updateButtons_);
    reload_();
}

QUuid OpenFilesDialog::current_() const
{
    const QList<QTableWidgetItem*> selected = m_table->selectedItems();
    return selected.isEmpty() ? QUuid() : selected.first()->data(Qt::UserRole).toUuid();
}

void OpenFilesDialog::reload_()
{
    const QUuid keep = current_();
    const QList<OpenFileService::Entry> entries = m_files->entries();
    m_table->setRowCount(int(entries.size()));
    for (int row = 0; row < entries.size(); ++row) {
        const OpenFileService::Entry& e = entries.at(row);
        const std::optional<Account> a = m_accounts ? m_accounts->account(e.account) : std::nullopt;
        auto* name = new QTableWidgetItem(e.key.section(u'/', -1));
        name->setToolTip(QDir::toNativeSeparators(e.path));
        auto* where = new QTableWidgetItem(QStringLiteral("%1 · %2/%3").arg(a ? a->name : QString(), e.bucket, e.key));
        auto* state = new QTableWidgetItem(stateText(e));
        state->setToolTip(state->text());
        for (QTableWidgetItem* item : {name, where, state})
            item->setData(Qt::UserRole, e.id);
        m_table->setItem(row, 0, name);
        m_table->setItem(row, 1, where);
        m_table->setItem(row, 2, state);
        if (e.id == keep)
            m_table->selectRow(row);
    }
    if (m_table->selectedItems().isEmpty() && !entries.isEmpty())
        m_table->selectRow(0);
    m_table->setVisible(!entries.isEmpty());
    m_empty->setVisible(entries.isEmpty());
    updateButtons_();
}

void OpenFilesDialog::updateButtons_()
{
    const OpenFileService::Entry* e = m_files->entry(current_());
    using S = OpenFileService::State;
    const bool changed = e && e->state == S::Changed && !e->readOnly;
    m_upload->setEnabled(changed);
    m_ignore->setEnabled(changed);
    m_again->setEnabled(e && (e->state == S::Open || e->state == S::Changed));
    m_folder->setEnabled(e && !e->path.isEmpty());
    m_discard->setEnabled(e && e->state != S::Uploading && e->state != S::Downloading);
    m_discard->setText(changed ? tr("Discard Changes…") : tr("Close File"));
}

} // namespace qbucket
