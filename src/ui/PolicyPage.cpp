#include "ui/PolicyPage.h"

#include "controllers/SetBucketConfigJob.h"
#include "ui/CodeEditor.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace qbucket {

PolicyPage::PolicyPage(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::Policy, context, parent)
{
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Template:")));
    m_templates = new QComboBox;
    m_templates->setObjectName(QStringLiteral("policyTemplates"));
    m_templates->addItem(tr("Choose a template…"));
    m_templates->addItem(tr("Public read of every object"));
    m_templates->addItem(tr("Deny requests without TLS"));
    m_templates->addItem(tr("Allow only from an IP range"));
    row->addWidget(m_templates, 1);
    editorArea()->addLayout(row);
    m_editor = new CodeEditor(CodeHighlighter::Language::Json);
    m_editor->setObjectName(QStringLiteral("policyEditor"));
    m_editor->setPlaceholderText(tr("No policy is set. Write one, or start from a template."));
    editorArea()->addWidget(m_editor, 1);
    m_findings = new QLabel;
    m_findings->setObjectName(QStringLiteral("policyFindings"));
    m_findings->setWordWrap(true);
    m_findings->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningColor().name()));
    m_findings->setVisible(false);
    editorArea()->addWidget(m_findings);

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        updateFindings_();
        edited_();
    });
    connect(m_templates, &QComboBox::activated, this, [this](int index) {
        if (index > 0)
            m_editor->setContents(templateText(index));
        m_templates->setCurrentIndex(0);
    });
}

QString PolicyPage::templateText(int index) const
{
    const QString bucket = context().bucket;
    const QString objects = QStringLiteral("arn:aws:s3:::%1/*").arg(bucket);
    const QString whole = QStringLiteral("arn:aws:s3:::%1").arg(bucket);
    QJsonObject statement;
    switch (index) {
    case 1:
        statement = {{QStringLiteral("Sid"), QStringLiteral("PublicRead")},
                     {QStringLiteral("Effect"), QStringLiteral("Allow")},
                     {QStringLiteral("Principal"), QStringLiteral("*")},
                     {QStringLiteral("Action"), QJsonArray{QStringLiteral("s3:GetObject")}},
                     {QStringLiteral("Resource"), QJsonArray{objects}}};
        break;
    case 2:
        statement = {{QStringLiteral("Sid"), QStringLiteral("DenyInsecureTransport")},
                     {QStringLiteral("Effect"), QStringLiteral("Deny")},
                     {QStringLiteral("Principal"), QStringLiteral("*")},
                     {QStringLiteral("Action"), QStringLiteral("s3:*")},
                     {QStringLiteral("Resource"), QJsonArray{whole, objects}},
                     {QStringLiteral("Condition"),
                      QJsonObject{{QStringLiteral("Bool"),
                                   QJsonObject{{QStringLiteral("aws:SecureTransport"), QStringLiteral("false")}}}}}};
        break;
    case 3:
        statement = {{QStringLiteral("Sid"), QStringLiteral("AllowOnlyFromRange")},
                     {QStringLiteral("Effect"), QStringLiteral("Deny")},
                     {QStringLiteral("Principal"), QStringLiteral("*")},
                     {QStringLiteral("Action"), QStringLiteral("s3:*")},
                     {QStringLiteral("Resource"), QJsonArray{whole, objects}},
                     {QStringLiteral("Condition"),
                      QJsonObject{{QStringLiteral("NotIpAddress"),
                                   QJsonObject{{QStringLiteral("aws:SourceIp"), QStringLiteral("192.0.2.0/24")}}}}}};
        break;
    default:
        return {};
    }
    const QJsonObject policy{{QStringLiteral("Version"), QStringLiteral("2012-10-17")},
                             {QStringLiteral("Statement"), QJsonArray{statement}}};
    return QString::fromUtf8(QJsonDocument(policy).toJson(QJsonDocument::Indented)).trimmed();
}

void PolicyPage::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_editor->setContents(doc.configured ? SetBucketConfigJob::pretty(kind(), doc.body) : QString());
    updateFindings_();
}

QByteArray PolicyPage::editedDocument_() const
{
    return m_editor->contents();
}

void PolicyPage::setEditable_(bool editable)
{
    m_editor->setReadOnly(!editable);
    m_templates->setEnabled(editable);
}

void PolicyPage::updateFindings_()
{
    const QByteArray json = m_editor->contents();
    QStringList lines;
    if (!json.trimmed().isEmpty() && s3::policyProblems(json).isEmpty()) {
        const s3::PolicyFindings f = s3::analyzePolicy(json);
        if (f.publicWrite)
            lines.append(tr("Anyone on the internet may write or delete with this policy."));
        else if (f.publicRead)
            lines.append(tr("Anyone on the internet may read what this policy names."));
        if (f.deny)
            lines.append(tr("A Deny statement can lock out these credentials too."));
    }
    m_findings->setText(lines.join(u'\n'));
    m_findings->setVisible(!lines.isEmpty());
}

} // namespace qbucket
