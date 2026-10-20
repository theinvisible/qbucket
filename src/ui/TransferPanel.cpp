#include "ui/TransferPanel.h"

#include "models/TransferModel.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace qbucket {

namespace {

// A thin progress bar in the theme's colours, with the text over it.
class ProgressDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();
        // Background and selection only; the text comes below, over the bar.
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const Palette& pal = Theme::current();
        const int permille = index.data(TransferModel::ProgressRole).toInt();
        const QRect bar = option.rect.adjusted(6, option.rect.height() - 8, -6, -4);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(pal.panel3);
        painter->drawRoundedRect(bar, 2, 2);
        if (permille > 0) {
            QRect done = bar;
            done.setWidth(std::max(2, bar.width() * permille / 1000));
            const bool finished = index.data(TransferModel::FinishedRole).toBool();
            painter->setBrush(finished && permille >= 1000 ? Theme::successColor() : pal.accent);
            painter->drawRoundedRect(done, 2, 2);
        }
        painter->setPen(opt.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(option.rect.adjusted(6, 1, -6, -9), Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(text, Qt::ElideRight, option.rect.width() - 12));
        painter->restore();
    }
};

QToolButton* makeButton(const QString& text)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setEnabled(false);
    return b;
}

} // namespace

TransferPanel::TransferPanel(TransferModel* model, QWidget* parent)
    : QWidget(parent),
      m_model(model)
{
    setObjectName(QStringLiteral("transferPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    m_pause = makeButton(tr("Pause"));
    m_resume = makeButton(tr("Resume"));
    m_cancel = makeButton(tr("Cancel"));
    m_retry = makeButton(tr("Retry Failed"));
    m_clear = makeButton(tr("Clear Finished"));
    for (QToolButton* b : {m_pause, m_resume, m_cancel, m_retry})
        bar->addWidget(b);
    bar->addStretch(1);
    bar->addWidget(m_clear);
    lay->addLayout(bar);

    m_stack = new QStackedWidget;
    auto* empty = new QLabel(tr("No transfers yet. Upload files with the toolbar, or download a selection."));
    empty->setObjectName(QStringLiteral("emptyState"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    m_stack->addWidget(empty);
    m_view = new QTableView;
    m_view->setObjectName(QStringLiteral("transferView"));
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setDefaultSectionSize(30);
    // Interactive with fixed widths: ResizeToContents is O(rows) per resize.
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->setColumnWidth(TransferModel::NameColumn, 360);
    m_view->setColumnWidth(TransferModel::ProgressColumn, 200);
    m_view->setColumnWidth(TransferModel::SpeedColumn, 100);
    m_view->setColumnWidth(TransferModel::RemainingColumn, 110);
    m_view->setItemDelegateForColumn(TransferModel::ProgressColumn, new ProgressDelegate(m_view));
    m_stack->addWidget(m_view);
    lay->addWidget(m_stack, 1);

    const auto refresh = [this] {
        m_stack->setCurrentIndex(m_model->rowCount() > 0 ? 1 : 0);
        updateButtons_();
    };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, refresh);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, refresh);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &TransferPanel::updateButtons_);
    connect(m_model, &TransferModel::transferStarted, this, [this] {
        // The newest transfer is the one to watch.
        if (m_view->selectionModel() && !m_view->selectionModel()->hasSelection())
            m_view->selectRow(m_model->rowCount() - 1);
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &TransferPanel::updateButtons_);
    connect(m_pause, &QToolButton::clicked, this, [this] { m_model->pause(currentRow_()); });
    connect(m_resume, &QToolButton::clicked, this, [this] { m_model->resume(currentRow_()); });
    connect(m_cancel, &QToolButton::clicked, this, [this] { m_model->cancel(currentRow_()); });
    connect(m_retry, &QToolButton::clicked, this, [this] { m_model->retry(currentRow_()); });
    connect(m_clear, &QToolButton::clicked, m_model, &TransferModel::clearFinished);
    refresh();
}

int TransferPanel::currentRow_() const
{
    const QModelIndexList rows = m_view->selectionModel() ? m_view->selectionModel()->selectedRows() : QModelIndexList{};
    return rows.isEmpty() ? -1 : rows.first().row();
}

void TransferPanel::updateButtons_()
{
    const int row = currentRow_();
    const QModelIndex i = row >= 0 ? m_model->index(row, 0) : QModelIndex();
    const bool finished = i.isValid() && i.data(TransferModel::FinishedRole).toBool();
    const bool paused = i.isValid() && i.data(TransferModel::PausedRole).toBool();
    m_pause->setEnabled(i.isValid() && i.data(TransferModel::CanPauseRole).toBool() && !paused);
    m_resume->setEnabled(paused);
    m_cancel->setEnabled(i.isValid() && !finished);
    m_retry->setEnabled(i.isValid() && i.data(TransferModel::CanRetryRole).toBool());
    bool anyFinished = false;
    for (int r = 0; r < m_model->rowCount() && !anyFinished; ++r)
        anyFinished = m_model->index(r, 0).data(TransferModel::FinishedRole).toBool();
    m_clear->setEnabled(anyFinished);
}

} // namespace qbucket
