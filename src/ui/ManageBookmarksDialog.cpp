#include "ui/ManageBookmarksDialog.h"

#include "accounts/LocationStore.h"
#include "models/BookmarkModel.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>

namespace qbucket {

ManageBookmarksDialog::ManageBookmarksDialog(LocationStore* store, AccountStore* accounts, QWidget* parent)
    : QDialog(parent),
      m_store(store),
      m_model(new BookmarkModel(store, accounts, this))
{
    setWindowTitle(tr("Manage Bookmarks"));
    setMinimumSize(520, 360);
    auto* lay = new QVBoxLayout(this);

    auto* row = new QHBoxLayout;
    m_view = new QListView;
    m_view->setObjectName(QStringLiteral("bookmarks"));
    m_view->setModel(m_model);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    row->addWidget(m_view, 1);
    auto* side = new QVBoxLayout;
    m_up = new QPushButton(tr("Move Up"));
    m_down = new QPushButton(tr("Move Down"));
    m_remove = new QPushButton(tr("Remove"));
    m_remove->setObjectName(QStringLiteral("danger"));
    side->addWidget(m_up);
    side->addWidget(m_down);
    side->addSpacing(12);
    side->addWidget(m_remove);
    side->addStretch(1);
    row->addLayout(side);
    lay->addLayout(row, 1);

    auto* form = new QFormLayout;
    m_name = new QLineEdit;
    m_place = new QLabel;
    m_place->setObjectName(QStringLiteral("fieldLabel"));
    m_place->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Location:"), m_place);
    lay->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(m_up, &QPushButton::clicked, this, [this] { move_(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { move_(+1); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int row = currentRow_();
        if (row < 0)
            return;
        m_store->removeBookmark(m_model->bookmarkAt(row).id);
        selectRow_(std::min(row, m_model->rowCount() - 1));
    });
    // A name is taken when editing ends (Enter, focus elsewhere).
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const int row = currentRow_();
        if (row < 0)
            return;
        const QUuid id = m_model->bookmarkAt(row).id;
        if (m_store->renameBookmark(id, m_name->text()))
            selectRow_(m_model->rowOf(id));
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &ManageBookmarksDialog::updateState_);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ManageBookmarksDialog::updateState_);
    selectRow_(0);
    updateState_();
}

int ManageBookmarksDialog::currentRow_() const
{
    const QModelIndex current = m_view->currentIndex();
    return current.isValid() ? current.row() : -1;
}

void ManageBookmarksDialog::selectRow_(int row)
{
    if (row >= 0 && row < m_model->rowCount())
        m_view->setCurrentIndex(m_model->index(row));
}

void ManageBookmarksDialog::move_(int delta)
{
    const int row = currentRow_();
    if (row < 0)
        return;
    const QUuid id = m_model->bookmarkAt(row).id;
    if (m_store->moveBookmark(id, row + delta))
        selectRow_(m_model->rowOf(id));
}

void ManageBookmarksDialog::updateState_()
{
    const int row = currentRow_();
    const bool any = row >= 0;
    m_name->setEnabled(any);
    m_up->setEnabled(any && row > 0);
    m_down->setEnabled(any && row + 1 < m_model->rowCount());
    m_remove->setEnabled(any);
    m_name->setText(any ? m_model->bookmarkAt(row).name : QString());
    m_place->setText(any ? m_model->data(m_model->index(row), BookmarkModel::PlaceRole).toString() : QString());
}

} // namespace qbucket
