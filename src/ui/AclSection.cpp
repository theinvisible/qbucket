#include "ui/AclSection.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QString granteeText(const s3::Grant& g)
{
    static const QString groups = QStringLiteral("http://acs.amazonaws.com/groups/global/");
    switch (g.grantee) {
    case s3::Grant::Grantee::Group:
        if (g.uri == groups + QStringLiteral("AllUsers"))
            return AclSection::tr("Everyone (public)");
        if (g.uri == groups + QStringLiteral("AuthenticatedUsers"))
            return AclSection::tr("Any authenticated account");
        return g.uri;
    case s3::Grant::Grantee::Email:
        return g.email;
    case s3::Grant::Grantee::CanonicalUser:
        break;
    }
    return g.displayName.isEmpty() ? g.id : QStringLiteral("%1 (%2)").arg(g.displayName, g.id);
}

} // namespace

AclSection::AclSection(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::Acl, context, parent)
{
    auto* form = new QFormLayout;
    m_canned = new QComboBox;
    m_canned->setObjectName(QStringLiteral("bucketCannedAcl"));
    m_canned->addItem(tr("Private"), QStringLiteral("private"));
    m_canned->addItem(tr("Public read (anyone may list)"), QStringLiteral("public-read"));
    m_canned->addItem(tr("Public read and write"), QStringLiteral("public-read-write"));
    m_canned->addItem(tr("Authenticated read"), QStringLiteral("authenticated-read"));
    form->addRow(tr("Canned ACL:"), m_canned);
    editorArea()->addLayout(form);
    m_grants = new QTreeWidget;
    m_grants->setObjectName(QStringLiteral("bucketGrants"));
    m_grants->setHeaderLabels({tr("Grantee"), tr("Permission")});
    m_grants->setRootIsDecorated(false);
    m_grants->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_grants->setColumnWidth(0, 300);
    m_grants->setMaximumHeight(120);
    editorArea()->addWidget(m_grants);
    connect(m_canned, &QComboBox::currentIndexChanged, this, [this] { edited_(); });
}

void AclSection::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_grants->clear();
    m_loadedCanned.clear();
    const s3::Result<s3::AccessControlPolicy> acl = s3::parseAccessControlPolicyDocument(doc.body);
    if (acl) {
        for (const s3::Grant& g : acl->grants)
            m_grants->addTopLevelItem(new QTreeWidgetItem({granteeText(g), g.permission}));
        m_loadedCanned = s3::cannedAclOf(*acl).value_or(QString());
    }
    const int index = m_canned->findData(m_loadedCanned);
    if (index < 0 && m_canned->findData(QString()) < 0)
        m_canned->addItem(tr("(own grants)"), QString());
    m_canned->setCurrentIndex(index >= 0 ? index : m_canned->findData(QString()));
    setNote_(m_loadedCanned.isEmpty() && acl
                 ? tr("These grants are no canned ACL: choosing one replaces them.")
                 : QString());
}

QString AclSection::stateText_(const s3::BucketConfigDocument& doc) const
{
    const s3::Result<s3::AccessControlPolicy> acl = s3::parseAccessControlPolicyDocument(doc.body);
    if (!acl)
        return tr("Not readable");
    const std::optional<QString> canned = s3::cannedAclOf(*acl);
    if (!canned)
        return tr("Own grants");
    const int index = m_canned->findData(*canned);
    return index >= 0 ? m_canned->itemText(index) : *canned;
}

QString AclSection::editedCannedAcl_() const
{
    return m_canned->currentData().toString();
}

void AclSection::setEditable_(bool editable)
{
    m_canned->setEnabled(editable);
}

QString AclSection::problem_() const
{
    return editedCannedAcl_().isEmpty() ? tr("Choose a canned ACL.") : QString();
}

bool AclSection::differs_() const
{
    return editedCannedAcl_() != m_loadedCanned;
}

} // namespace qbucket
