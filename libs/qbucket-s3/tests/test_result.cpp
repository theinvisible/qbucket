#include <qbucket/s3/Result.h>

#include <QStringList>
#include <QtTest>

#include <memory>

using namespace qbucket::s3;

namespace {

Result<int> parsePositive(const QString& text)
{
    bool ok = false;
    const int v = text.toInt(&ok);
    if (!ok || v <= 0)
        return Error::make(ErrorCategory::InvalidRequest, QStringLiteral("not a positive number"));
    return v;
}

Result<void> mustBeEmpty(const QString& text)
{
    if (!text.isEmpty())
        return Error::make(ErrorCategory::Conflict, QStringLiteral("not empty"));
    return {};
}

} // namespace

class TestResult : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void valueAndError()
    {
        const Result<int> ok = parsePositive(QStringLiteral("42"));
        QVERIFY(ok);
        QVERIFY(ok.has_value());
        QCOMPARE(ok.value(), 42);
        QCOMPARE(*ok, 42);

        const Result<int> bad = parsePositive(QStringLiteral("x"));
        QVERIFY(!bad);
        QCOMPARE(bad.error().category, ErrorCategory::InvalidRequest);
        QCOMPARE(bad.value_or(-1), -1);
    }

    void moveOnlyValue()
    {
        Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
        QVERIFY(r);
        std::unique_ptr<int> p = std::move(r).value();
        QCOMPARE(*p, 7);
    }

    void arrowOnContainers()
    {
        Result<QStringList> r = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        QCOMPARE(r->size(), 2);
        r->append(QStringLiteral("c"));
        QCOMPARE(r.value().size(), 3);
    }

    void transformMapsValueAndPassesErrors()
    {
        const Result<int> ok = 20;
        const Result<QString> s = ok.transform([](int v) { return QString::number(v * 2); });
        QCOMPARE(s.value(), QStringLiteral("40"));

        const Result<int> bad = Error::make(ErrorCategory::NotFound, QStringLiteral("gone"));
        const Result<QString> s2 = bad.transform([](int v) { return QString::number(v); });
        QVERIFY(!s2);
        QCOMPARE(s2.error().category, ErrorCategory::NotFound);
        QCOMPARE(s2.error().message, QStringLiteral("gone"));

        int seen = 0;
        const Result<void> v = ok.transform([&](int x) { seen = x; });
        QVERIFY(v);
        QCOMPARE(seen, 20);
    }

    void voidResult()
    {
        QVERIFY(mustBeEmpty(QString()));
        const Result<void> bad = mustBeEmpty(QStringLiteral("x"));
        QVERIFY(!bad);
        QCOMPARE(bad.error().category, ErrorCategory::Conflict);
    }

    void errorToString()
    {
        Error e;
        e.category = ErrorCategory::NotFound;
        e.httpStatus = 404;
        e.code = QStringLiteral("NoSuchKey");
        e.message = QStringLiteral("The specified key does not exist.");
        QCOMPARE(e.toString(),
                 QStringLiteral("NotFound (404 NoSuchKey): The specified key does not exist."));

        Error lost = Error::make(ErrorCategory::Transient, QStringLiteral("reply lost"));
        lost.outcomeUnknown = true;
        QCOMPARE(lost.toString(), QStringLiteral("Transient: reply lost [outcome unknown]"));
        QVERIFY(lost.isTransient());
    }

    void categoryNamesAreDistinct()
    {
        QSet<QString> names;
        for (int c = 0; c <= static_cast<int>(ErrorCategory::Internal); ++c)
            names.insert(Error::categoryName(static_cast<ErrorCategory>(c)));
        QCOMPARE(names.size(), static_cast<int>(ErrorCategory::Internal) + 1);
    }
};

QTEST_GUILESS_MAIN(TestResult)
#include "test_result.moc"
