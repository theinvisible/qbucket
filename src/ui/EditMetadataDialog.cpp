#include "ui/EditMetadataDialog.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace qbucket {

namespace {

enum Action { Leave = 0, Set = 1, Remove = 2 };

bool printableAscii(const QString& text)
{
    return std::all_of(text.begin(), text.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() <= 0x7E; });
}

} // namespace

EditMetadataDialog::EditMetadataDialog(const QString& key, const s3::ObjectMeta& current, QWidget* parent)
    : QDialog(parent),
      m_bulk(false),
      m_before(s3::objectHeadersOf(current))
{
    setWindowTitle(tr("Edit Metadata"));
    build_(tr("Headers and user metadata of “%1”. Only what you change is changed.").arg(key));
}

EditMetadataDialog::EditMetadataDialog(int objects, QWidget* parent)
    : QDialog(parent),
      m_bulk(true)
{
    setWindowTitle(tr("Edit Metadata"));
    build_(tr("For %n object(s) and everything in the selected folders: each header is left alone, set or "
              "removed — the rest of each object stays as it is.", nullptr, objects));
}

void EditMetadataDialog::build_(const QString& intro)
{
    auto* lay = new QVBoxLayout(this);
    auto* text = new QLabel(intro);
    text->setWordWrap(true);
    lay->addWidget(text);

    auto* form = new QFormLayout;
    const std::pair<const char*, QString s3::ObjectHeaders::*> headers[] = {
        {"Content-Type", &s3::ObjectHeaders::contentType},
        {"Cache-Control", &s3::ObjectHeaders::cacheControl},
        {"Content-Disposition", &s3::ObjectHeaders::contentDisposition},
        {"Content-Encoding", &s3::ObjectHeaders::contentEncoding},
        {"Content-Language", &s3::ObjectHeaders::contentLanguage},
        {"Expires", &s3::ObjectHeaders::expires},
        {"x-amz-website-redirect-location", &s3::ObjectHeaders::websiteRedirectLocation},
    };
    for (const auto& [name, member] : headers) {
        Row row;
        row.header = QLatin1String(name);
        row.member = member;
        row.edit = new QLineEdit;
        row.edit->setObjectName(QStringLiteral("header:") + row.header);
        auto* line = new QHBoxLayout;
        if (m_bulk) {
            row.action = new QComboBox;
            row.action->setObjectName(QStringLiteral("action:") + row.header);
            row.action->addItem(tr("leave"), Leave);
            row.action->addItem(tr("set to"), Set);
            row.action->addItem(tr("remove"), Remove);
            row.edit->setEnabled(false);
            QLineEdit* edit = row.edit;
            connect(row.action, &QComboBox::currentIndexChanged, this, [this, edit](int index) {
                edit->setEnabled(index == Set);
                validate_();
            });
            line->addWidget(row.action);
        } else {
            row.edit->setText(m_before.*member);
            row.edit->setPlaceholderText(tr("not set"));
        }
        connect(row.edit, &QLineEdit::textChanged, this, &EditMetadataDialog::validate_);
        line->addWidget(row.edit, 1);
        form->addRow(row.header, line);
        m_rows.append(row);
    }
    lay->addLayout(form);

    lay->addWidget(new QLabel(m_bulk ? tr("User metadata (x-amz-meta-*) — set, or remove when ticked:")
                                     : tr("User metadata (x-amz-meta-*):")));
    m_meta = new QTableWidget(0, m_bulk ? 3 : 2);
    m_meta->setObjectName(QStringLiteral("metadata"));
    m_meta->setHorizontalHeaderLabels(m_bulk ? QStringList{tr("Name"), tr("Value"), tr("Remove")}
                                             : QStringList{tr("Name"), tr("Value")});
    m_meta->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_meta->horizontalHeader()->setStretchLastSection(true);
    m_meta->setColumnWidth(0, 180);
    m_meta->verticalHeader()->hide();
    m_meta->setMinimumHeight(120);
    connect(m_meta, &QTableWidget::itemChanged, this, &EditMetadataDialog::validate_);
    lay->addWidget(m_meta, 1);
    auto* metaButtons = new QHBoxLayout;
    auto* add = new QToolButton;
    add->setText(tr("Add"));
    connect(add, &QToolButton::clicked, this, [this] {
        addMetadataRow(QString(), QString());
        m_meta->editItem(m_meta->item(m_meta->rowCount() - 1, 0));
    });
    auto* remove = new QToolButton;
    remove->setText(tr("Remove"));
    connect(remove, &QToolButton::clicked, this, [this] {
        if (m_meta->currentRow() >= 0)
            m_meta->removeRow(m_meta->currentRow());
        validate_();
    });
    metaButtons->addWidget(add);
    metaButtons->addWidget(remove);
    metaButtons->addStretch(1);
    lay->addLayout(metaButtons);
    if (!m_bulk) {
        for (auto it = m_before.userMetadata.cbegin(); it != m_before.userMetadata.cend(); ++it)
            addMetadataRow(it.key(), it.value());
    }

    m_problem = new QLabel;
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningColor().name()));
    lay->addWidget(m_problem);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue…"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(m_buttons);
    resize(620, 560);
    validate_();
}

void EditMetadataDialog::addMetadataRow(const QString& name, const QString& value, bool remove)
{
    const QSignalBlocker block(m_meta);
    const int row = m_meta->rowCount();
    m_meta->insertRow(row);
    m_meta->setItem(row, 0, new QTableWidgetItem(name));
    m_meta->setItem(row, 1, new QTableWidgetItem(value));
    if (m_bulk) {
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        check->setCheckState(remove ? Qt::Checked : Qt::Unchecked);
        m_meta->setItem(row, 2, check);
    }
    validate_();
}

QLineEdit* EditMetadataDialog::field(const QString& header) const
{
    for (const Row& r : m_rows) {
        if (r.header == header)
            return r.edit;
    }
    return nullptr;
}

QComboBox* EditMetadataDialog::action(const QString& header) const
{
    for (const Row& r : m_rows) {
        if (r.header == header)
            return r.action;
    }
    return nullptr;
}

QPushButton* EditMetadataDialog::okButton() const
{
    return m_buttons ? m_buttons->button(QDialogButtonBox::Ok) : nullptr;
}

s3::ObjectHeaders EditMetadataDialog::edited_() const
{
    s3::ObjectHeaders h = m_before;
    for (const Row& r : m_rows)
        h.*(r.member) = r.edit->text().trimmed();
    h.userMetadata.clear();
    for (int row = 0; row < m_meta->rowCount(); ++row) {
        const QString name = m_meta->item(row, 0) ? m_meta->item(row, 0)->text().trimmed().toLower() : QString();
        if (!name.isEmpty())
            h.userMetadata.insert(name, m_meta->item(row, 1) ? m_meta->item(row, 1)->text() : QString());
    }
    return h;
}

EditObjectsJob::ChangeSpec EditMetadataDialog::change() const
{
    if (!m_bulk)
        return EditObjectsJob::ChangeSpec::between(m_before, edited_());
    EditObjectsJob::ChangeSpec c;
    const auto pick = [](const Row& r) -> std::optional<QString> {
        switch (r.action->currentData().toInt()) {
        case Set:
            return r.edit->text().trimmed();
        case Remove:
            return QString();
        default:
            return std::nullopt;
        }
    };
    for (const Row& r : m_rows) {
        const std::optional<QString> v = pick(r);
        if (r.member == &s3::ObjectHeaders::contentType)
            c.contentType = v;
        else if (r.member == &s3::ObjectHeaders::cacheControl)
            c.cacheControl = v;
        else if (r.member == &s3::ObjectHeaders::contentDisposition)
            c.contentDisposition = v;
        else if (r.member == &s3::ObjectHeaders::contentEncoding)
            c.contentEncoding = v;
        else if (r.member == &s3::ObjectHeaders::contentLanguage)
            c.contentLanguage = v;
        else if (r.member == &s3::ObjectHeaders::expires)
            c.expires = v;
        else if (r.member == &s3::ObjectHeaders::websiteRedirectLocation)
            c.websiteRedirectLocation = v;
    }
    for (int row = 0; row < m_meta->rowCount(); ++row) {
        const QString name = m_meta->item(row, 0) ? m_meta->item(row, 0)->text().trimmed().toLower() : QString();
        if (name.isEmpty())
            continue;
        const bool remove = m_meta->item(row, 2) && m_meta->item(row, 2)->checkState() == Qt::Checked;
        c.metadata.insert(name, remove ? std::nullopt
                                       : std::optional<QString>(m_meta->item(row, 1) ? m_meta->item(row, 1)->text() : QString()));
    }
    return c;
}

void EditMetadataDialog::validate_()
{
    if (!m_buttons || !m_meta)
        return;
    QString problem;
    for (const Row& r : m_rows) {
        if (!printableAscii(r.edit->text()))
            problem = tr("%1 may only hold plain ASCII characters.").arg(r.header);
    }
    static const QRegularExpression metaName(QStringLiteral("^[A-Za-z0-9._-]+$"));
    QStringList names;
    for (int row = 0; row < m_meta->rowCount() && problem.isEmpty(); ++row) {
        const QString name = m_meta->item(row, 0) ? m_meta->item(row, 0)->text().trimmed() : QString();
        const QString value = m_meta->item(row, 1) ? m_meta->item(row, 1)->text() : QString();
        if (name.isEmpty() && value.isEmpty())
            continue;
        if (!metaName.match(name).hasMatch())
            problem = tr("Metadata names are letters, digits, “.”, “-” and “_”: “%1”.").arg(name);
        else if (!printableAscii(value))
            problem = tr("The value of “%1” may only hold plain ASCII characters.").arg(name);
        else if (names.contains(name.toLower()))
            problem = tr("“%1” appears twice.").arg(name);
        names.append(name.toLower());
    }
    if (problem.isEmpty() && change().isEmpty())
        problem = m_bulk ? tr("Choose what to change.") : QString();
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    okButton()->setEnabled(problem.isEmpty() && !change().isEmpty());
}

} // namespace qbucket
