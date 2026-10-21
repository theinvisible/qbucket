// EditMetadataDialog: one object's values and the difference it makes; the
// bulk form (leave / set / remove); what it refuses to send.

#include "ui/EditMetadataDialog.h"
#include "ui/ThemeManager.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QtTest>

using namespace qbucket;

class TestEditMetadataDialog : public QObject {
    Q_OBJECT

private:
    ThemeManager m_theme;

private slots:
    void initTestCase() { m_theme.apply(QStringLiteral("Light")); }

    void oneObjectGivesTheDifference()
    {
        s3::ObjectMeta meta;
        meta.contentType = QStringLiteral("text/plain");
        meta.cacheControl = QStringLiteral("no-cache");
        meta.userMetadata = {{QStringLiteral("owner"), QStringLiteral("ops")}, {QStringLiteral("stage"), QStringLiteral("draft")}};
        EditMetadataDialog dlg(QStringLiteral("docs/a.txt"), meta);
        QCOMPARE(dlg.field(QStringLiteral("Content-Type"))->text(), QStringLiteral("text/plain"));
        QCOMPARE(dlg.metadataTable()->rowCount(), 2);
        QVERIFY(!dlg.okButton()->isEnabled());   // nothing changed yet

        dlg.field(QStringLiteral("Cache-Control"))->setText(QStringLiteral("max-age=60"));
        dlg.field(QStringLiteral("Content-Language"))->setText(QStringLiteral("de"));
        dlg.metadataTable()->item(1, 1)->setText(QStringLiteral("final"));   // stage
        dlg.metadataTable()->removeRow(0);                                  // owner
        QVERIFY(dlg.okButton()->isEnabled());
        const EditObjectsJob::ChangeSpec c = dlg.change();
        QVERIFY(!c.contentType);
        QCOMPARE(c.cacheControl, std::optional<QString>(QStringLiteral("max-age=60")));
        QCOMPARE(c.contentLanguage, std::optional<QString>(QStringLiteral("de")));
        QCOMPARE(c.metadata.value(QStringLiteral("stage")), std::optional<QString>(QStringLiteral("final")));
        QVERIFY(c.metadata.contains(QStringLiteral("owner")) && !c.metadata.value(QStringLiteral("owner")));

        // Not ASCII: refused before anything is sent.
        dlg.field(QStringLiteral("Content-Disposition"))->setText(QStringLiteral("attachment; filename=\"ü.txt\""));
        QVERIFY(!dlg.okButton()->isEnabled());
        dlg.field(QStringLiteral("Content-Disposition"))->clear();
        dlg.addMetadataRow(QStringLiteral("bad name"), QStringLiteral("x"));
        QVERIFY(!dlg.okButton()->isEnabled());
    }

    void severalObjectsLeaveSetOrRemove()
    {
        EditMetadataDialog dlg(3);
        QVERIFY(dlg.isBulk());
        QVERIFY(!dlg.okButton()->isEnabled());   // nothing chosen
        QVERIFY(!dlg.field(QStringLiteral("Cache-Control"))->isEnabled());
        dlg.action(QStringLiteral("Cache-Control"))->setCurrentIndex(1);   // set to
        QVERIFY(dlg.field(QStringLiteral("Cache-Control"))->isEnabled());
        dlg.field(QStringLiteral("Cache-Control"))->setText(QStringLiteral("max-age=5"));
        dlg.action(QStringLiteral("Expires"))->setCurrentIndex(2);         // remove
        dlg.addMetadataRow(QStringLiteral("team"), QStringLiteral("web"));
        dlg.addMetadataRow(QStringLiteral("old"), QString(), true);
        QVERIFY(dlg.okButton()->isEnabled());
        const EditObjectsJob::ChangeSpec c = dlg.change();
        QCOMPARE(c.cacheControl, std::optional<QString>(QStringLiteral("max-age=5")));
        QCOMPARE(c.expires, std::optional<QString>(QString()));
        QVERIFY(!c.contentType);
        QCOMPARE(c.metadata.value(QStringLiteral("team")), std::optional<QString>(QStringLiteral("web")));
        QVERIFY(c.metadata.contains(QStringLiteral("old")) && !c.metadata.value(QStringLiteral("old")));
    }
};

QTEST_MAIN(TestEditMetadataDialog)
#include "test_edit_metadata_dialog.moc"
