#include "ui/PublicAccessBlockSection.h"

#include <QCheckBox>
#include <QVBoxLayout>

namespace qbucket {

PublicAccessBlockSection::PublicAccessBlockSection(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::PublicAccessBlock, context, parent)
{
    m_blockAcls = new QCheckBox(tr("Block new public ACLs"));
    m_ignoreAcls = new QCheckBox(tr("Ignore existing public ACLs"));
    m_blockPolicy = new QCheckBox(tr("Block new public bucket policies"));
    m_restrict = new QCheckBox(tr("Restrict access through public policies to the owner's account"));
    m_blockAcls->setObjectName(QStringLiteral("pabBlockAcls"));
    for (QCheckBox* c : {m_blockAcls, m_ignoreAcls, m_blockPolicy, m_restrict}) {
        editorArea()->addWidget(c);
        connect(c, &QCheckBox::toggled, this, [this] { edited_(); });
    }
}

s3::PublicAccessBlockConfiguration PublicAccessBlockSection::edited() const
{
    s3::PublicAccessBlockConfiguration c;
    c.blockPublicAcls = m_blockAcls->isChecked();
    c.ignorePublicAcls = m_ignoreAcls->isChecked();
    c.blockPublicPolicy = m_blockPolicy->isChecked();
    c.restrictPublicBuckets = m_restrict->isChecked();
    return c;
}

void PublicAccessBlockSection::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_loaded = {};
    if (doc.configured) {
        if (const s3::Result<s3::PublicAccessBlockConfiguration> c = s3::parsePublicAccessBlock(doc.body))
            m_loaded = *c;
    }
    m_blockAcls->setChecked(m_loaded.blockPublicAcls);
    m_ignoreAcls->setChecked(m_loaded.ignorePublicAcls);
    m_blockPolicy->setChecked(m_loaded.blockPublicPolicy);
    m_restrict->setChecked(m_loaded.restrictPublicBuckets);
}

QByteArray PublicAccessBlockSection::editedDocument_() const
{
    return s3::buildPublicAccessBlock(edited());
}

void PublicAccessBlockSection::setEditable_(bool editable)
{
    for (QCheckBox* c : {m_blockAcls, m_ignoreAcls, m_blockPolicy, m_restrict})
        c->setEnabled(editable);
}

bool PublicAccessBlockSection::differs_() const
{
    s3::PublicAccessBlockConfiguration now = edited();
    now.lossless = m_loaded.lossless;
    return !(now == m_loaded);
}

} // namespace qbucket
