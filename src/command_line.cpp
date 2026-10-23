#include "command_line.h"
#include <QCoreApplication>
#include <QKeyEvent>

namespace ivia {
CommandLine::CommandLine(QWidget *parent) : QLineEdit(parent)
{
    setFrame(false);
    setTextMargins(0, 0, 0, 0);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setClearButtonEnabled(false);
}

bool CommandLine::physicalControl(Qt::KeyboardModifiers modifiers)
{
    auto control = Qt::ControlModifier;
    auto other = Qt::MetaModifier;
#ifdef Q_OS_MACOS
    if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) {
        control = Qt::MetaModifier;
        other = Qt::ControlModifier;
    }
#endif
    return modifiers.testFlag(control) && !modifiers.testFlag(other)
            && !modifiers.testFlag(Qt::AltModifier);
}

bool CommandLine::handles(const QKeyEvent *key) const
{
    if (physicalControl(key->modifiers())) {
        switch (key->key()) {
        case Qt::Key_A: case Qt::Key_B: case Qt::Key_C: case Qt::Key_D:
        case Qt::Key_E: case Qt::Key_H: case Qt::Key_I: case Qt::Key_J:
        case Qt::Key_L: case Qt::Key_M: case Qt::Key_N: case Qt::Key_P:
        case Qt::Key_U: case Qt::Key_W: case Qt::Key_BracketLeft:
            return true;
        default: return false;
        }
    }
    if (key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))
        return false; // Let native Command shortcuts and composed text through.
    if (m_completionActive && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) return true;
    return key->key() == Qt::Key_Escape || key->key() == Qt::Key_Backspace
            || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab
            || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down;
}

bool CommandLine::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (handles(key)) {
            // Reserve the key before application shortcuts run; edit only once,
            // on KeyPress. Native input methods keep handling printable text.
            if (event->type() == QEvent::KeyPress) handleKey(key);
            event->accept();
            return true;
        }
    }
    return QLineEdit::event(event);
}

void CommandLine::erasePreviousWord()
{
    if (hasSelectedText()) { insert({}); return; }
    const auto value = text();
    const int end = cursorPosition();
    int start = end;
    while (start > 0 && value.at(start - 1).isSpace()) --start;
    if (start > 0) {
        auto keyword = [](QChar ch) { return ch.isLetterOrNumber() || ch.isMark() || ch == '_'; };
        const bool word = keyword(value.at(start - 1));
        while (start > 0 && !value.at(start - 1).isSpace() && keyword(value.at(start - 1)) == word)
            --start;
    }
    setSelection(start, end - start);
    insert({});
}

void CommandLine::handleKey(QKeyEvent *key)
{
    if (physicalControl(key->modifiers())) {
        switch (key->key()) {
        case Qt::Key_A: emit allCompletionsRequested(); break;
        case Qt::Key_B: home(false); break;
        case Qt::Key_C: case Qt::Key_BracketLeft: emit cancelRequested(); break;
        case Qt::Key_D: emit completionListRequested(); break;
        case Qt::Key_E: end(false); break;
        case Qt::Key_H:
            if (text().isEmpty()) emit cancelRequested(); else backspace();
            break;
        case Qt::Key_I: emit completionRequested(false); break;
        case Qt::Key_J: case Qt::Key_M: {
            // Use Qt's native Return path to commit an input method's pending
            // preedit and apply the same validation as the Enter key.
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QLineEdit::keyPressEvent(&enter);
            break;
        }
        case Qt::Key_L: emit longestCompletionRequested(); break;
        case Qt::Key_N: emit historyRequested(1, true); break;
        case Qt::Key_P: emit historyRequested(-1, true); break;
        case Qt::Key_U:
            setSelection(0, cursorPosition()); insert({}); break;
        case Qt::Key_W: erasePreviousWord(); break;
        default: break;
        }
        return;
    }
    switch (key->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: QLineEdit::keyPressEvent(key); break;
    case Qt::Key_Escape: emit cancelRequested(); break;
    case Qt::Key_Backspace:
        if (text().isEmpty()) emit cancelRequested(); else backspace();
        break;
    case Qt::Key_Tab: case Qt::Key_Backtab:
        emit completionRequested(key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier));
        break;
    case Qt::Key_Left: emit completionRequested(true); break;
    case Qt::Key_Right: emit completionRequested(false); break;
    case Qt::Key_Up: emit historyRequested(-1, false); break;
    case Qt::Key_Down: emit historyRequested(1, false); break;
    default: break;
    }
}
}
