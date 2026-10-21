#include "ui/CodeEditor.h"

#include <QEvent>
#include <QFontDatabase>

namespace qbucket {

CodeEditor::CodeEditor(CodeHighlighter::Language language, QWidget* parent)
    : QPlainTextEdit(parent),
      m_highlighter(new CodeHighlighter(language, document()))
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 2);
    setTabChangesFocus(false);
}

void CodeEditor::setContents(const QString& text)
{
    if (text != toPlainText())
        setPlainText(text);
}

void CodeEditor::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        m_highlighter->refreshColours();
}

} // namespace qbucket
