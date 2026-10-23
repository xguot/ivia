#pragma once
#include <QLineEdit>

class QKeyEvent;

namespace ivia {
class CommandLine final : public QLineEdit {
    Q_OBJECT
public:
    explicit CommandLine(QWidget *parent = nullptr);
    static bool physicalControl(Qt::KeyboardModifiers modifiers);
    void setCompletionActive(bool active) { m_completionActive = active; }

signals:
    void cancelRequested();
    void completionRequested(bool reverse);
    void completionListRequested();
    void allCompletionsRequested();
    void longestCompletionRequested();
    void historyRequested(int direction, bool cycleCompletion);

protected:
    bool event(QEvent *event) override;

private:
    bool handles(const QKeyEvent *key) const;
    void handleKey(QKeyEvent *key);
    void erasePreviousWord();
    bool m_completionActive = false;
};
}
