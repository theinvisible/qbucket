#include "controllers/BrowseController.h"

#include <QSignalSpy>
#include <QtTest>

using namespace qbucket;

namespace {

Location at(const QUuid& account, const char* bucket, const char* prefix = "")
{
    return {account, QString::fromLatin1(bucket), QString::fromLatin1(prefix)};
}

} // namespace

class TestBrowseController : public QObject {
    Q_OBJECT

private slots:
    void locationHelpers()
    {
        const QUuid a = QUuid::createUuid();
        QVERIFY(!Location{}.isValid());
        QVERIFY(!(Location{a, {}, {}}.hasBucket()));
        QCOMPARE(at(a, "b", "x/y/").parent(), at(a, "b", "x/"));
        QCOMPARE(at(a, "b", "x/").parent(), at(a, "b"));
        QCOMPARE(at(a, "b").parent(), at(a, "b"));
        QCOMPARE(at(a, "b", "x//y/").parent(), at(a, "b", "x//"));
        QCOMPARE(at(a, "b", "x//y/").segments(),
                 (QStringList{QStringLiteral("x"), QString(), QStringLiteral("y")}));
        QVERIFY(at(a, "b").segments().isEmpty());
    }

    void backForwardUp()
    {
        const QUuid a = QUuid::createUuid();
        BrowseController nav;
        QSignalSpy changed(&nav, &BrowseController::locationChanged);
        QVERIFY(!nav.canGoBack());

        nav.navigate(at(a, "b"));
        nav.navigate(at(a, "b", "x/"));
        nav.navigate(at(a, "b", "x/y/"));
        QCOMPARE(changed.size(), 3);
        nav.navigate(at(a, "b", "x/y/"));   // same place: nothing
        QCOMPARE(changed.size(), 3);
        QVERIFY(nav.canGoBack());
        QVERIFY(!nav.canGoForward());
        QVERIFY(nav.canGoUp());

        nav.back();
        QCOMPARE(nav.current(), at(a, "b", "x/"));
        QVERIFY(nav.canGoForward());
        nav.back();
        QCOMPARE(nav.current(), at(a, "b"));
        QVERIFY(!nav.canGoUp());
        nav.forward();
        QCOMPARE(nav.current(), at(a, "b", "x/"));

        nav.navigate(at(a, "b", "x/z/"));   // a new branch drops forward
        QVERIFY(!nav.canGoForward());
        nav.up();
        QCOMPARE(nav.current(), at(a, "b", "x/"));
        nav.back();
        QCOMPARE(nav.current(), at(a, "b", "x/z/"));
        QCOMPARE(changed.last().at(0).value<Location>(), at(a, "b", "x/z/"));
    }

    void historyIsBounded()
    {
        const QUuid a = QUuid::createUuid();
        BrowseController nav;
        for (int i = 0; i < BrowseController::kMaxHistory + 50; ++i)
            nav.navigate({a, QStringLiteral("b"), QStringLiteral("p%1/").arg(i)});
        int steps = 0;
        while (nav.canGoBack()) {
            nav.back();
            ++steps;
        }
        QCOMPARE(steps, BrowseController::kMaxHistory);
    }

    void forgettingAnAccount()
    {
        const QUuid a = QUuid::createUuid();
        const QUuid gone = QUuid::createUuid();
        BrowseController nav;
        nav.navigate(at(a, "one"));
        nav.navigate(at(gone, "x"));
        nav.navigate(at(a, "one"));
        nav.navigate(at(gone, "y"));
        nav.forgetAccount(gone);
        QVERIFY(!nav.current().isValid());
        QVERIFY(nav.canGoBack());
        nav.back();
        QCOMPARE(nav.current(), at(a, "one"));
        QVERIFY(!nav.canGoBack());   // the duplicate "one" collapsed
    }
};

QTEST_GUILESS_MAIN(TestBrowseController)
#include "test_browse_controller.moc"
