#include "ui/CorsPage.h"

#include "controllers/SetBucketConfigJob.h"
#include "ui/CodeEditor.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QStringList listOf(const QString& text)
{
    static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
    return text.split(separators, Qt::SkipEmptyParts);
}

QString cell(const QTableWidget* table, int row, int column)
{
    const QTableWidgetItem* item = table->item(row, column);
    return item ? item->text().trimmed() : QString();
}

} // namespace

CorsPage::CorsPage(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::Cors, context, parent)
{
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    auto* rules = new QWidget;
    auto* rulesLay = new QVBoxLayout(rules);
    rulesLay->setContentsMargins(0, 6, 0, 0);
    m_table = new QTableWidget(0, ColumnCount);
    m_table->setObjectName(QStringLiteral("corsRules"));
    m_table->setHorizontalHeaderLabels({tr("ID"), tr("Origins"), tr("Methods"), tr("Allowed headers"),
                                        tr("Exposed headers"), tr("Max age (s)")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(IdColumn, 90);
    m_table->setColumnWidth(OriginsColumn, 170);
    m_table->setColumnWidth(MethodsColumn, 110);
    m_table->setColumnWidth(HeadersColumn, 110);
    m_table->setColumnWidth(ExposeColumn, 110);
    m_table->setToolTip(tr("Lists are separated by commas, e.g. “GET, PUT”."));
    rulesLay->addWidget(m_table, 1);
    auto* row = new QHBoxLayout;
    m_add = new QPushButton(tr("Add Rule"));
    m_removeRule = new QPushButton(tr("Remove Rule"));
    row->addWidget(m_add);
    row->addWidget(m_removeRule);
    row->addStretch(1);
    rulesLay->addLayout(row);
    m_tabs->addTab(rules, tr("Rules"));
    m_xml = new CodeEditor(CodeHighlighter::Language::Xml);
    m_xml->setObjectName(QStringLiteral("corsXml"));
    m_xml->setPlaceholderText(tr("No CORS rules are set."));
    m_tabs->addTab(m_xml, tr("XML"));
    editorArea()->addWidget(m_tabs, 1);

    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_syncing)
            tableToXml_();
    });
    connect(m_xml, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_syncing)
            xmlToTable_();
        edited_();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        m_syncing = true;
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        m_table->setItem(r, OriginsColumn, new QTableWidgetItem(QStringLiteral("https://example.com")));
        m_table->setItem(r, MethodsColumn, new QTableWidgetItem(QStringLiteral("GET")));
        m_syncing = false;
        tableToXml_();
        m_table->setCurrentCell(r, OriginsColumn);
    });
    connect(m_removeRule, &QPushButton::clicked, this, [this] {
        if (m_table->currentRow() < 0)
            return;
        m_table->removeRow(m_table->currentRow());
        tableToXml_();
    });
}

void CorsPage::fillTable_(const s3::CorsConfiguration& config)
{
    m_syncing = true;
    m_table->setRowCount(0);
    for (const s3::CorsRule& rule : config.rules) {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        m_table->setItem(r, IdColumn, new QTableWidgetItem(rule.id));
        m_table->setItem(r, OriginsColumn, new QTableWidgetItem(rule.allowedOrigins.join(QStringLiteral(", "))));
        m_table->setItem(r, MethodsColumn, new QTableWidgetItem(rule.allowedMethods.join(QStringLiteral(", "))));
        m_table->setItem(r, HeadersColumn, new QTableWidgetItem(rule.allowedHeaders.join(QStringLiteral(", "))));
        m_table->setItem(r, ExposeColumn, new QTableWidgetItem(rule.exposeHeaders.join(QStringLiteral(", "))));
        m_table->setItem(r, MaxAgeColumn,
                         new QTableWidgetItem(rule.maxAgeSeconds ? QString::number(*rule.maxAgeSeconds) : QString()));
    }
    m_syncing = false;
}

void CorsPage::tableToXml_()
{
    s3::CorsConfiguration config;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        s3::CorsRule rule;
        rule.id = cell(m_table, r, IdColumn);
        rule.allowedOrigins = listOf(cell(m_table, r, OriginsColumn));
        rule.allowedMethods = listOf(cell(m_table, r, MethodsColumn).toUpper());
        rule.allowedHeaders = listOf(cell(m_table, r, HeadersColumn));
        rule.exposeHeaders = listOf(cell(m_table, r, ExposeColumn));
        bool ok = false;
        const int age = cell(m_table, r, MaxAgeColumn).toInt(&ok);
        if (ok)
            rule.maxAgeSeconds = age;
        config.rules.append(rule);
    }
    m_syncing = true;
    if (config.rules.isEmpty()) {
        m_xml->setContents(QString());
    } else {
        const s3::Result<QByteArray> xml = s3::buildCorsConfiguration(config);
        if (xml)
            m_xml->setContents(SetBucketConfigJob::pretty(kind(), *xml));
    }
    m_syncing = false;
    edited_();
}

void CorsPage::xmlToTable_()
{
    const QByteArray xml = m_xml->contents();
    if (xml.trimmed().isEmpty()) {
        m_tableUsable = true;
        fillTable_({});
    } else if (const s3::Result<s3::CorsConfiguration> c = s3::parseCorsConfiguration(xml)) {
        m_tableUsable = c->lossless;
        if (c->lossless)
            fillTable_(*c);
    }
    // Unparsable for now (being typed): the table keeps what it showed.
    setNote_(m_tableUsable ? QString()
                           : tr("These rules have parts the table can't show: edit them as XML."));
    setEditable_(m_editable);
}

void CorsPage::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_syncing = true;
    m_xml->setContents(doc.configured ? SetBucketConfigJob::pretty(kind(), doc.body) : QString());
    m_syncing = false;
    xmlToTable_();
    if (!m_tableUsable)
        m_tabs->setCurrentIndex(1);
}

QByteArray CorsPage::editedDocument_() const
{
    return m_xml->contents();
}

void CorsPage::setEditable_(bool editable)
{
    m_editable = editable;
    const bool table = editable && m_tableUsable;
    m_table->setEditTriggers(table ? QAbstractItemView::AllEditTriggers : QAbstractItemView::NoEditTriggers);
    m_add->setEnabled(table);
    m_removeRule->setEnabled(table);
    m_xml->setReadOnly(!editable);
}

} // namespace qbucket
