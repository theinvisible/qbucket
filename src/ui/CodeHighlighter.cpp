#include "ui/CodeHighlighter.h"

#include "ui/Theme.h"

namespace qbucket {

namespace {

QTextCharFormat colour(const QColor& c, bool bold = false)
{
    QTextCharFormat f;
    f.setForeground(c);
    if (bold)
        f.setFontWeight(QFont::DemiBold);
    return f;
}

} // namespace

CodeHighlighter::CodeHighlighter(Language language, QTextDocument* document)
    : QSyntaxHighlighter(document),
      m_language(language)
{
    refreshColours();
}

void CodeHighlighter::refreshColours()
{
    const Palette& p = Theme::current();
    const QColor accent = p.accent;
    const QColor string = Theme::successColor();
    const QColor number = Theme::warningColor();
    const QColor muted = p.text3;
    m_rules.clear();
    if (m_language == Language::Json) {
        m_rules.append({QRegularExpression(QStringLiteral(R"("(?:[^"\\]|\\.)*")")), colour(string)});
        // A key: a string followed by a colon (wins over the plain string).
        m_rules.append({QRegularExpression(QStringLiteral(R"(("(?:[^"\\]|\\.)*")\s*:)")), colour(accent, true), 1});
        m_rules.append({QRegularExpression(QStringLiteral(R"(\b-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)")), colour(number)});
        m_rules.append({QRegularExpression(QStringLiteral(R"(\b(?:true|false|null)\b)")), colour(number, true)});
    } else {
        m_rules.append({QRegularExpression(QStringLiteral(R"(</?[\w:.-]+|/?>)")), colour(accent, true)});
        m_rules.append({QRegularExpression(QStringLiteral(R"(\s([\w:.-]+)=)")), colour(number), 1});
        m_rules.append({QRegularExpression(QStringLiteral(R"("[^"]*"|'[^']*')")), colour(string)});
        m_rules.append({QRegularExpression(QStringLiteral(R"(<!--.*?-->|<\?.*?\?>)")), colour(muted)});
    }
    rehighlight();
}

void CodeHighlighter::apply_(const Rule& rule, const QString& text)
{
    QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        setFormat(int(m.capturedStart(rule.group)), int(m.capturedLength(rule.group)), rule.format);
    }
}

void CodeHighlighter::highlightBlock(const QString& text)
{
    for (const Rule& rule : std::as_const(m_rules))
        apply_(rule, text);
}

} // namespace qbucket
