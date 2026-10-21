#pragma once

#include "ui/CodeHighlighter.h"

#include <QPlainTextEdit>

namespace qbucket {

// A plain text editor for JSON / XML documents: fixed-width font, no
// wrapping, two-space tabs, highlighting with the theme's colours.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(CodeHighlighter::Language language, QWidget* parent = nullptr);

    [[nodiscard]] QByteArray contents() const { return toPlainText().toUtf8(); }
    void setContents(const QString& text);

protected:
    void changeEvent(QEvent* event) override;

private:
    CodeHighlighter* m_highlighter;
};

} // namespace qbucket
