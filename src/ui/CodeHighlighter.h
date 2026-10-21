#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace qbucket {

// Colours JSON (keys, strings, numbers, literals) or XML (tags, attributes,
// values, comments) with the theme's tokens. Re-reads them on rehighlight().
class CodeHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    enum class Language { Json, Xml };

    CodeHighlighter(Language language, QTextDocument* document);

    [[nodiscard]] Language language() const { return m_language; }
    // After a theme switch.
    void refreshColours();

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
        int group = 0;
    };
    void apply_(const Rule& rule, const QString& text);

    Language m_language;
    QList<Rule> m_rules;
};

} // namespace qbucket
