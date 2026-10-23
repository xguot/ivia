#pragma once
#include "reader_view.h"
#include <QMainWindow>
#include <QMap>
#include <QTimer>
#include <memory>
#include <vector>

class QLineEdit;
class QLabel;
class QStackedWidget;
class QTreeWidget;
class QFrame;
class QPlainTextEdit;
class QPdfDocument;
class QPdfBookmarkModel;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;

namespace ivia {
class CommandLine;
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool openFile(const QString &path);
    bool executeCommand(const QString &text);
    ReaderView *reader() const;
    int bufferCount() const { return int(m_buffers.size()); }
    void restoreSession();
    void showMessage(const QString &message);

protected:
    bool eventFilter(QObject *, QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    struct Buffer {
        int number = 0;
        QString path;
        QPdfDocument *document = nullptr;
        ReaderView *view = nullptr;
        QMap<QChar, Location> marks;
    };
    Buffer *buffer() const;
    ReaderView *createView(QPdfDocument *document);
    void buildUi();
    void applyTheme();
    void chooseFile();
    bool chooseFiles(const QString &directory);
    void switchBuffer(int index);
    void closeBuffer();
    void saveBuffer(Buffer *buffer);
    void saveSession();
    void updateStatus();
    void positionOverlays();
    void enterPrompt(QChar kind, QString initial = {});
    void leavePrompt();
    void submitPrompt();
    void completePrompt(bool reverse = false);
    QStringList promptCompletions() const;
    void listPromptCompletions();
    void insertAllCompletions();
    void longestCompletion();
    void replacePromptCompletion(const QString &prefix);
    void recallPrompt(int direction, bool cycleCompletion);
    void showCompletionMenu(const QStringList &candidates, int selected = -1);
    void showBuffers();
    void dismissOutput();
    bool changeDirectory(const QString &argument);
    QString bufferName(const Buffer *item) const;
    void normalKey(QKeyEvent *event);
    void resetPending();
    void showPanel(const QString &kind);
    void populatePanel();
    void showHelp();
    void preview(int page, QPointF point);
    void setMark(QChar mark);
    void jumpMark(QChar mark);
    QString baseDirectory() const;

    std::vector<std::unique_ptr<Buffer>> m_buffers;
    int m_active = -1;
    int m_nextBufferNumber = 1;
    QString m_alternatePath;
    QStringList m_bufferVisits;
    QString m_workingDirectory;
    QString m_previousDirectory;
    QStackedWidget *m_stack;
    QWidget *m_welcome;
    QLabel *m_messageLabel;
    QLabel *m_pendingLabel;
    QStackedWidget *m_bottomLine;
    QFrame *m_statusLine;
    QLabel *m_statusFile;
    QLabel *m_statusPosition;
    QLabel *m_continueLabel;
    QPlainTextEdit *m_output;
    bool m_waitingForEnter = false;
    QFrame *m_promptBar;
    QLabel *m_promptPrefix;
    CommandLine *m_prompt;
    QLabel *m_completionLabel;
    QWidget *m_panel;
    QLabel *m_panelTitle;
    QTreeWidget *m_panelTree;
    QString m_panelKind;
    QChar m_promptKind;
    QString m_pending;
    QString m_count;
    QStringList m_history;
    int m_historyIndex = 0;
    QString m_historyDraft;
    QStringList m_completions;
    QString m_completionText;
    QString m_completionOriginal;
    int m_completionIndex = -1;
    int m_completionCursor = -1;
    QTimer m_messageTimer;
    QTimer m_saveTimer;
    bool m_dark = false;
    bool m_restoring = false;
};
}
