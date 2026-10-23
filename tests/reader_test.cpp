#include "command.h"
#include "command_line.h"
#include "demo.h"
#include "main_window.h"
#include <QFile>
#include <QFileDialog>
#include <QClipboard>
#include <QLineEdit>
#include <QLabel>
#include <QAction>
#include <QInputMethodEvent>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QPdfDocument>
#include <QPdfPageRenderer>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <memory>

using namespace ivia;

#ifdef Q_OS_MACOS
constexpr auto vimControl = Qt::MetaModifier;
#else
constexpr auto vimControl = Qt::ControlModifier;
#endif

// A tiny, deterministic PDF with an internal link, an outline and searchable
// numbered references. No network fixture or third-party document is needed.
static bool writeFixture(const QString &path)
{
    const QList<QByteArray> streams = {
        "BT /F1 14 Tf 72 700 Td (Attention and reference [1]) Tj 0 -40 Td (Jump to references) Tj ET",
        "BT /F1 14 Tf 72 700 Td (Second page: attention and evidence.) Tj ET",
        "BT /F1 14 Tf 72 700 Td ([1] A reference about attention.) Tj 0 -40 Td ([2] Another reference.) Tj ET"
    };
    QList<QByteArray> objects = {
        "<< /Type /Catalog /Pages 2 0 R /Outlines 11 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R] /Count 3 >>"
    };
    for (int page = 0; page < 3; ++page) {
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 9 0 R >> >> /Contents "
                       + QByteArray::number(4 + page * 2) + " 0 R "
                       + (page == 0 ? "/Annots [10 0 R] " : "") + ">>");
        objects.append("<< /Length " + QByteArray::number(streams[page].size()) + ">>\nstream\n" + streams[page] + "\nendstream");
    }
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    objects.append("<< /Type /Annot /Subtype /Link /Rect [72 650 200 675] /Border [0 0 0] /Dest [7 0 R /XYZ 0 720 0] >>");
    objects.append("<< /Type /Outlines /First 12 0 R /Last 12 0 R /Count 1 >>");
    objects.append("<< /Title (References) /Parent 11 0 R /Dest [7 0 R /XYZ 0 720 0] >>");
    QByteArray data = "%PDF-1.4\n";
    QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(data.size());
        data += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = data.size();
    data += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i)
        data += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    data += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

class ReaderTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(m_directory.isValid());
        QCoreApplication::setOrganizationName("IviaTests");
        QCoreApplication::setApplicationName("IviaTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_directory.path());
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        m_pdf = m_directory.filePath("Paper with spaces.pdf");
        m_other = m_directory.filePath("Second paper.pdf");
        QVERIFY(writeFixture(m_pdf));
        QVERIFY(writeFixture(m_other));
    }
    void init() { QSettings().clear(); }
    void parsePaths() {
        const auto command = parseCommand(":e  \"~/Papers/a paper.pdf\"  ");
        QCOMPARE(command.name, "e");
        QCOMPARE(command.argument, "~/Papers/a paper.pdf");
        QCOMPARE(parseCommand("e a paper.pdf").argument, "a paper.pdf");
        QCOMPARE(expandPath("../paper.pdf", "/tmp/papers"), "/tmp/paper.pdf");
        QVERIFY(expandPath("~/paper.pdf").startsWith(QDir::homePath()));
    }
    void failedOpenKeepsBuffer() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        auto *original = window.reader();
        QVERIFY(!window.openFile(m_directory.filePath("missing.pdf")));
        QCOMPARE(window.reader(), original);
        QCOMPARE(window.bufferCount(), 1);
        QVERIFY(window.openFile(m_pdf));
        QCOMPARE(window.bufferCount(), 1);
        QFile invalid(m_directory.filePath("broken.pdf"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("invalid"); invalid.close();
        QVERIFY(!window.openFile(invalid.fileName()));
        QCOMPARE(window.reader(), original);
    }
    void keysAndPrompt() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QTest::qWait(80);
        auto *view = window.reader();
        const int start = view->verticalScrollBar()->value();
        QTest::keyClicks(view, "3j");
        QCOMPARE(view->verticalScrollBar()->value(), start + 144);
        QTest::keyClicks(view, "2gg");
        QCOMPARE(view->currentPage(), 1);
        QTest::keyClicks(view, "gg");
        QCOMPARE(view->currentPage(), 0);
        QTest::keyClick(view, Qt::Key_D, vimControl);
        QVERIFY(view->verticalScrollBar()->value() > 100);
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QVERIFY(prompt->isVisible());
        QTest::keyClicks(prompt, "zoom 125");
        QTest::keyClick(prompt, Qt::Key_Return);
        QCOMPARE(view->zoom(), 1.25);
        QVERIFY(!view->fitsWidth());
        QTest::keyClicks(view, "0"); // 0 is not a normal-mode fit shortcut
        QCOMPARE(view->zoom(), 1.25);
        QVERIFY(!view->fitsWidth());
        QTest::keyClicks(view, "W");
        QVERIFY(view->fitsWidth());
        const qreal widthZoom = view->zoom();
        QTest::keyClicks(view, "H");
        QVERIFY(!view->fitsWidth());
        QVERIFY(view->zoom() != widthZoom);
        QTest::keyClicks(view, "=");
        QVERIFY(view->zoom() > 0.1);
        QTest::keyClicks(view, "-");
        QTest::keyClicks(view, "+");
        QTest::keyClicks(view, "W");
        QVERIFY(view->fitsWidth());
        QVERIFY(!prompt->isVisible());
        QTest::keyClicks(view, ":");
        QTest::keyClick(prompt, Qt::Key_Up);
        QCOMPARE(prompt->text(), "zoom 125");
        QTest::keyClick(prompt, Qt::Key_Escape);
        QVERIFY(!prompt->isVisible());
        QVERIFY(!window.executeCommand("zoom nan"));
        QVERIFY(!window.executeCommand("page 999"));
    }
    void promptOverlaysCanvasAndCyclesCompletions() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QVERIFY(window.openFile(m_other));
        QTest::qWait(40);
        auto *view = window.reader();
        QCOMPARE(view->mapTo(window.centralWidget(), QPoint()), QPoint());
        auto *bottom = window.findChild<QStackedWidget *>("bottomLine");
        QCOMPARE(view->width(), window.centralWidget()->width());
        QCOMPARE(view->height() + bottom->height(), window.centralWidget()->height());
        view->scrollBy(0, 120);
        const auto viewportSize = view->viewport()->size();
        const auto pageOrigin = view->viewportPoint(0, {});
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QVERIFY(prompt->isVisible());
        QVERIFY(prompt->placeholderText().isEmpty());
        QCOMPARE(view->viewport()->size(), viewportSize);
        QCOMPARE(view->viewportPoint(0, {}), pageOrigin);
        QVERIFY(window.executeCommand("cd \"" + m_directory.path() + "\""));
        QTest::keyClicks(prompt, "b ");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "b \"Paper with spaces.pdf\"");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "b \"Second paper.pdf\"");
        QTest::keyClick(prompt, Qt::Key_Backtab);
        QCOMPARE(prompt->text(), "b \"Paper with spaces.pdf\"");
        QTest::keyClick(prompt, Qt::Key_Return);
        QVERIFY(window.reader() != view);
        auto *active = window.reader();
        QTest::keyClicks(active, "/");
        const auto searchSize = active->viewport()->size();
        window.resize(720, 540);
        QTest::qWait(30);
        QVERIFY(window.centralWidget()->rect().contains(prompt->parentWidget()->geometry()));
        QTest::keyClick(prompt, Qt::Key_Escape);
        QVERIFY(!prompt->isVisible());
        QVERIFY(active->viewport()->size() != searchSize); // only the actual window resize changes it
        QVERIFY(window.executeCommand("info"));
        auto *notice = window.findChild<QLabel *>("notice");
        QVERIFY(notice->isVisible());
        QTest::keyClick(active, Qt::Key_Escape);
        QVERIFY(!notice->isVisible());
    }
    void commandLineEditingAndInputMethods() {
        MainWindow window;
        window.show();
        QTest::keyClicks(window.centralWidget(), ":");
        auto *prompt = window.findChild<CommandLine *>("commandLine");
        QVERIFY(prompt->isVisible());
        auto *line = prompt->parentWidget();
        QCOMPARE(line->x(), 0);
        QCOMPARE(line->width(), window.centralWidget()->width());
        QCOMPARE(line->mapTo(window.centralWidget(), QPoint()).y() + line->height(), window.centralWidget()->height());
        QCOMPARE(window.findChild<QLabel *>("promptPrefix")->mapTo(line, QPoint()).x(), 12);
        QVERIFY(line->height() <= 28);

        int shortcutCount = 0;
        auto *competing = new QAction(&window);
        competing->setShortcut(QKeySequence(vimControl | Qt::Key_H));
        window.addAction(competing);
        connect(competing, &QAction::triggered, &window, [&] { ++shortcutCount; });
        QTest::keyClicks(prompt, "e ~/papers/x");
        QTest::keyClick(prompt, Qt::Key_H, vimControl);
        QCOMPARE(prompt->text(), "e ~/papers/");
        QCOMPARE(shortcutCount, 0);
        prompt->setText("e alpha beta");
        QTest::keyClick(prompt, Qt::Key_W, vimControl);
        QCOMPARE(prompt->text(), "e alpha ");
        prompt->setText("e ~/papers/file.pdf");
        QTest::keyClick(prompt, Qt::Key_B, vimControl);
        QCOMPARE(prompt->cursorPosition(), 0);
        QTest::keyClick(prompt, Qt::Key_E, vimControl);
        QCOMPARE(prompt->cursorPosition(), prompt->text().size());
        prompt->setCursorPosition(4);
        QTest::keyClick(prompt, Qt::Key_U, vimControl);
        QCOMPARE(prompt->text(), "papers/file.pdf");
        QCOMPARE(prompt->cursorPosition(), 0);

        prompt->clear();
        QInputMethodEvent composed;
        composed.setCommitString("~/");
        QApplication::sendEvent(prompt, &composed);
        QCOMPARE(prompt->text(), "~/");
#ifdef Q_OS_MACOS
        QTest::keyClick(prompt, Qt::Key_A, Qt::ControlModifier); // native Command+A
        QCOMPARE(prompt->selectedText(), "~/");
#endif
        QTest::keyClick(prompt, Qt::Key_C, vimControl);
        QVERIFY(!prompt->isVisible());
        QTest::keyClicks(window.centralWidget(), ":");
        QTest::keyClick(prompt, Qt::Key_H, vimControl);
        QVERIFY(!prompt->isVisible());
        QTest::keyClicks(window.centralWidget(), "/");
        QTest::keyClicks(prompt, "queryx");
        QTest::keyClick(prompt, Qt::Key_H, vimControl);
        QCOMPARE(prompt->text(), "query");
        QTest::keyClick(prompt, Qt::Key_BracketLeft, vimControl);
        QVERIFY(!prompt->isVisible());
    }
    void vimCompletionHistoryAndHomePaths() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        auto *view = window.reader();
        view->jump(1);
        const auto before = view->verticalScrollBar()->value();
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<CommandLine *>("commandLine");
        const auto promptPage = view->currentPage();
        QTest::keyClick(prompt, Qt::Key_O, vimControl);
        QCOMPARE(view->currentPage(), promptPage);
        prompt->setText("b");
        QTest::keyClick(prompt, Qt::Key_D, vimControl);
        QCOMPARE(prompt->text(), "b");
        auto *list = window.findChild<QLabel *>("completionList");
        QVERIFY(list->isVisible());
        QVERIFY(list->text().contains("Paper with spaces.pdf"));
        QCOMPARE(view->verticalScrollBar()->value(), before);
        QTest::keyClicks(prompt, "n");
        QVERIFY(!list->isVisible());
        prompt->setText("zo 125");
        prompt->setCursorPosition(2);
        QTest::keyClick(prompt, Qt::Key_L, vimControl);
        QCOMPARE(prompt->text(), "zoom 125");
        QCOMPARE(prompt->cursorPosition(), 4);
        QTest::keyClick(prompt, Qt::Key_M, vimControl);
        QCOMPARE(view->zoom(), 1.25);
        QVERIFY(!prompt->isVisible());

        QTest::keyClicks(view, ":");
        prompt->setText("draft");
        QTest::keyClick(prompt, Qt::Key_P, vimControl);
        QCOMPARE(prompt->text(), "zoom 125");
        QTest::keyClick(prompt, Qt::Key_N, vimControl);
        QCOMPARE(prompt->text(), "draft");
        prompt->setText("z");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "zoom");
        QTest::keyClick(prompt, Qt::Key_Escape);
        QTest::keyClicks(view, ":");
        prompt->setText("fi");
        QTest::keyClick(prompt, Qt::Key_D, vimControl);
        QTest::keyClick(prompt, Qt::Key_N, vimControl);
        QCOMPARE(prompt->text(), "fit");
        QTest::keyClick(prompt, Qt::Key_P, vimControl);
        QCOMPARE(prompt->text(), "fi");
        QTest::keyClick(prompt, Qt::Key_A, vimControl);
        QCOMPARE(prompt->text(), "fit files");

        QVERIFY(writeFixture(m_directory.filePath("cycle-a.pdf")));
        QVERIFY(writeFixture(m_directory.filePath("cycle-b.pdf")));
        const auto original = "e " + m_directory.filePath("cycle-");
        prompt->setText(original);
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "e " + m_directory.filePath("cycle-a.pdf"));
        QTest::keyClick(prompt, Qt::Key_P, vimControl);
        QCOMPARE(prompt->text(), original);

        prompt->setText("e ~");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "e ~/");
        const auto homeRelative = "~/" + QDir(QDir::homePath()).relativeFilePath(m_pdf);
        prompt->setText("e " + homeRelative.left(homeRelative.size() - 4));
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "e " + homeRelative);
        QTest::keyClick(prompt, Qt::Key_J, vimControl);
        QCOMPARE(window.bufferCount(), 1); // Expanded home path finds the same buffer.
        QVERIFY(!prompt->isVisible());
        QTest::keyClicks(view, ":");
        prompt->setText("e \"" + m_other.left(m_other.size() - 4));
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "e \"" + m_other + '"');
        QTest::keyClick(prompt, Qt::Key_Return);
        QCOMPARE(window.bufferCount(), 2);
    }
    void directoryCommandsBrowseExpandedHome() {
        MainWindow window;
        window.show();
        QString displayedDirectory;
        QTimer closer;
        closer.setInterval(10);
        connect(&closer, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                displayedDirectory = dialog->directory().absolutePath();
                dialog->reject();
                closer.stop();
            }
        });
        closer.start();
        QVERIFY(!window.executeCommand("e ~")); // Cancel the picker; don't open a document.
        closer.stop();
        QCOMPARE(displayedDirectory, QDir::homePath());
        QCOMPARE(window.bufferCount(), 0);
    }
    void buffersKeepIndependentState() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QTest::qWait(40);
        auto *first = window.reader();
        first->setZoom(1.4);
        first->jump(1);
        QVERIFY(window.openFile(m_other));
        QCOMPARE(window.bufferCount(), 2);
        QVERIFY(window.reader() != first);
        QVERIFY(window.executeCommand("b 1"));
        QCOMPARE(window.reader(), first);
        QCOMPARE(first->zoom(), 1.4);
        QCOMPARE(first->currentPage(), 1);
        QVERIFY(!window.executeCommand("b paper")); // ambiguous
        QVERIFY(window.executeCommand("bd"));
        QCOMPARE(window.bufferCount(), 1);
        QVERIFY(!window.executeCommand("b #")); // a closed buffer cannot remain its own alternate
        QVERIFY(window.executeCommand("bd"));
        QCOMPARE(window.bufferCount(), 0);
        QCOMPARE(window.reader(), nullptr);
    }
    void statusRowAndVimBufferOutput() {
        MainWindow window;
        window.show();
        QVERIFY(window.executeCommand("cd " + m_directory.path()));
        QVERIFY(window.openFile(m_pdf));
        QVERIFY(window.openFile(m_other));
        auto *view = window.reader();
        const auto size = view->viewport()->size();
        auto *bottom = window.findChild<QStackedWidget *>("bottomLine");
        auto *status = window.findChild<QFrame *>("statusLine");
        auto *filename = window.findChild<QLabel *>("statusFile");
        QCOMPARE(bottom->currentWidget(), status);
        QCOMPARE(filename->text(), "Second paper.pdf");
        QVERIFY(window.findChild<QLabel *>("statusPosition")->text().startsWith("1/3"));
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<CommandLine *>("commandLine");
        QVERIFY(prompt->isVisible());
        QVERIFY(!status->isVisible());
        QCOMPARE(view->viewport()->size(), size);
        QTest::keyClicks(prompt, "ls");
        QTest::keyClick(prompt, Qt::Key_Return);
        auto *output = window.findChild<QPlainTextEdit *>("commandOutput");
        QVERIFY(output->isVisible());
        QVERIFY(!window.findChild<QWidget *>("panel")->isVisible());
        const auto listing = output->toPlainText();
        QVERIFY(listing.contains("1 #h"));
        QVERIFY(listing.contains("2 %a"));
        QVERIFY(listing.contains("\"Paper with spaces.pdf\""));
        QVERIFY(listing.contains("page 1"));
        QVERIFY(window.findChild<QLabel *>("hitEnter")->isVisible());
        QCOMPARE(view->viewport()->size(), size);
        QTest::keyClick(window.centralWidget(), Qt::Key_Return);
        QVERIFY(!output->isVisible());
        QCOMPARE(bottom->currentWidget(), status);
        QVERIFY(window.executeCommand("ls"));
        QTest::keyClicks(window.centralWidget(), ":");
        QTest::keyClicks(prompt, "b #");
        QVERIFY(!output->isVisible());
        QCOMPARE(prompt->text(), "b #");
        QTest::keyClick(prompt, Qt::Key_Return);
        QVERIFY(window.reader() != view);
        QCOMPARE(filename->text(), "Paper with spaces.pdf");
        auto *first = window.reader();
        QVERIFY(window.executeCommand("b"));
        QCOMPARE(window.reader(), first);
    }
    void relativeBufferCompletionAndStableNumbers() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir("alpha"));
        QVERIFY(QDir(root.path()).mkdir("beta"));
        const auto a = root.filePath("alpha/paper.pdf");
        const auto b = root.filePath("beta/paper.pdf");
        QVERIFY(QFile::copy(m_pdf, a));
        QVERIFY(QFile::copy(m_pdf, b));
        MainWindow window;
        window.show();
        QVERIFY(window.executeCommand("cd " + root.path()));
        QVERIFY(window.openFile(a));
        auto *first = window.reader();
        QVERIFY(window.openFile(b));
        QTest::keyClicks(window.reader(), ":");
        auto *prompt = window.findChild<CommandLine *>("commandLine");
        QTest::keyClicks(prompt, "b");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "b alpha/paper.pdf");
        auto *choices = window.findChild<QLabel *>("completionList");
        QVERIFY(choices->isVisible());
        QVERIFY(choices->text().contains("alpha/paper.pdf"));
        QVERIFY(choices->text().contains("beta/paper.pdf"));
        QTest::keyClick(prompt, Qt::Key_Right);
        QCOMPARE(prompt->text(), "b beta/paper.pdf");
        QTest::keyClick(prompt, Qt::Key_Left);
        QCOMPARE(prompt->text(), "b alpha/paper.pdf");
        QTest::keyClick(prompt, Qt::Key_Return);
        QCOMPARE(window.reader(), first);
        QVERIFY(!window.executeCommand("b paper.pdf"));
        QVERIFY(window.openFile(m_other)); // stable buffer 3
        QVERIFY(window.executeCommand("b 1"));
        QVERIFY(window.executeCommand("bd"));
        QVERIFY(!window.executeCommand("b 1"));
        QVERIFY(window.executeCommand("b 3"));
        QVERIFY(window.executeCommand("ls"));
        const auto listing = window.findChild<QPlainTextEdit *>("commandOutput")->toPlainText();
        QVERIFY(listing.contains("3 %a"));
        QVERIFY(listing.contains("2 #h"));
        QVERIFY(!listing.contains("1 "));
    }
    void workingDirectoryCommandsAndSession() {
        const auto processDirectory = QDir::currentPath();
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir("papers"));
        const auto paper = root.filePath("papers/paper.pdf");
        QVERIFY(QFile::copy(m_pdf, paper));
        {
            MainWindow window;
            window.show();
            QVERIFY(window.executeCommand("cd " + root.path()));
            QVERIFY(window.executeCommand("pwd"));
            auto *message = window.findChild<QLabel *>("notice");
            QCOMPARE(message->toolTip(), QFileInfo(root.path()).canonicalFilePath());
            QVERIFY(window.executeCommand("e papers/paper.pdf"));
            auto *first = window.reader();
            QVERIFY(window.openFile(m_other));
            QVERIFY(window.executeCommand("pwd"));
            QCOMPARE(message->toolTip(), QFileInfo(root.path()).canonicalFilePath()); // switching files doesn't cd
            QTest::keyClicks(window.reader(), ":");
            auto *prompt = window.findChild<CommandLine *>("commandLine");
            QTest::keyClicks(prompt, "cd pa");
            QTest::keyClick(prompt, Qt::Key_Tab);
            QCOMPARE(prompt->text(), "cd papers/");
            QTest::keyClick(prompt, Qt::Key_Return);
            QVERIFY(window.executeCommand("b paper.pdf"));
            QCOMPARE(window.reader(), first);
            QVERIFY(window.executeCommand("pwd"));
            QCOMPARE(message->toolTip(), QFileInfo(root.filePath("papers")).canonicalFilePath());
            QVERIFY(!window.executeCommand("cd missing"));
            QVERIFY(window.executeCommand("pwd"));
            QCOMPARE(message->toolTip(), QFileInfo(root.filePath("papers")).canonicalFilePath());
            QVERIFY(window.executeCommand("cd -"));
            QVERIFY(window.executeCommand("pwd"));
            QCOMPARE(message->toolTip(), QFileInfo(root.path()).canonicalFilePath());
            QVERIFY(window.executeCommand("cd"));
            QVERIFY(window.executeCommand("pwd"));
            QCOMPARE(message->toolTip(), QFileInfo(QDir::homePath()).canonicalFilePath());
            QVERIFY(window.executeCommand("cd -"));
            window.close();
        }
        MainWindow restored;
        restored.restoreSession();
        QVERIFY(restored.executeCommand("pwd"));
        QCOMPARE(restored.findChild<QLabel *>("notice")->toolTip(), QFileInfo(root.path()).canonicalFilePath());
        QCOMPARE(restored.bufferCount(), 2);
        QCOMPARE(QDir::currentPath(), processDirectory); // no process-global mutation
    }
    void reloadIsTransactional() {
        const auto path = m_directory.filePath("Reload.pdf");
        QVERIFY(writeFixture(path));
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(path));
        QVERIFY(window.executeCommand("cd " + m_directory.path()));
        QTest::qWait(30);
        auto *original = window.reader();
        original->setZoom(1.2);
        original->jump(1);
        QVERIFY(window.executeCommand("mark r"));
        QFile broken(path);
        QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        broken.write("incomplete rebuild");
        broken.close();
        QVERIFY(!window.executeCommand("reload"));
        QCOMPARE(window.reader(), original);
        QCOMPARE(original->currentPage(), 1);
        QVERIFY(writeFixture(path));
        QVERIFY(window.executeCommand("reload"));
        QVERIFY(window.reader() != original);
        QTest::qWait(30);
        QCOMPARE(window.reader()->currentPage(), 1);
        QCOMPARE(window.reader()->zoom(), 1.2);
        QCOMPARE(window.bufferCount(), 1);
        window.reader()->jump(0);
        QTest::keyClicks(window.reader(), "'r");
        QCOMPARE(window.reader()->currentPage(), 1);
    }
    void marksAndSession() {
        {
            MainWindow window;
            window.show();
            QVERIFY(window.openFile(m_pdf));
            QTest::qWait(40);
            auto *view = window.reader();
            view->jump(1);
            QTest::keyClicks(view, "ma");
            view->jump(0);
            QTest::keyClicks(view, "'a");
            QCOMPARE(view->currentPage(), 1);
            window.close();
        }
        MainWindow restored;
        restored.restoreSession();
        QCOMPARE(restored.bufferCount(), 1);
        QCOMPARE(restored.reader()->currentPage(), 1);
        restored.show();
        QTest::qWait(50);
        QCOMPARE(restored.reader()->currentPage(), 1);
        restored.reader()->jump(0);
        QTest::keyClicks(restored.reader(), "'a");
        QCOMPARE(restored.reader()->currentPage(), 1);
    }
    void linksReferencesAndHistory() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QTest::qWait(40);
        auto *view = window.reader();
        view->beginHints();
        QVERIFY(view->hinting());
        QVERIFY(view->hintKey("a"));
        QVERIFY(!view->hinting());
        QCOMPARE(view->currentPage(), 2);
        view->back();
        QCOMPARE(view->currentPage(), 0);
        view->forward();
        QCOMPARE(view->currentPage(), 2);
        view->jump(0);
        view->findReference("[2]");
        QTRY_COMPARE_WITH_TIMEOUT(view->currentPage(), 2, 3000);
        view->back();
        QCOMPARE(view->currentPage(), 0);
        QVERIFY(window.executeCommand("toc"));
        auto *tree = window.findChild<QTreeWidget *>("panelTree");
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(0), "References");
    }
    void selectionAndPositionRestoration() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QTest::qWait(40);
        auto *view = window.reader();
        const auto start = view->viewportPoint(0, QPointF(72, 87)).toPoint();
        const auto end = view->viewportPoint(0, QPointF(132, 87)).toPoint();
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view->viewport(), end, 20);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(!view->selectedText().isEmpty());
        QApplication::clipboard()->clear();
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier); // native copy, including Command+C on macOS
        QCOMPARE(QApplication::clipboard()->text(), view->selectedText());
        view->clearSelection();
        QVERIFY(view->selectedText().isEmpty());
        const int y = qRound(view->pageRect(0).bottom()) - 80;
        view->verticalScrollBar()->setValue(y);
        const auto location = view->location();
        view->jump(2);
        view->restore(location);
        QCOMPARE(view->verticalScrollBar()->value(), y);
        view->setZoom(1.2);
        QVERIFY(window.executeCommand("mark z"));
        view->setZoom(0.7);
        QTest::keyClicks(view, "'z");
        QCOMPARE(view->zoom(), 1.2);

        // Alacritty-style Vi copy mode via Ctrl+Shift+Space.
        view->jump(0);
        auto *pending = window.findChild<QLabel *>("pendingKeys");
        QVERIFY(!view->viMode());
        QTest::keyClick(view, Qt::Key_Space, vimControl | Qt::ShiftModifier);
        QVERIFY(view->viMode());
        QCOMPARE(pending->text(), "-- VI --");
        QTest::keyClicks(view, "0v8l");
        QVERIFY(view->viSelecting());
        QCOMPARE(pending->text(), "-- VISUAL --");
        QVERIFY(view->selectedText().startsWith("Attention"));
        QApplication::clipboard()->clear();
        QTest::keyClicks(view, "y");
        QCOMPARE(QApplication::clipboard()->text(), "Attention");
        QVERIFY(view->viMode());
        QVERIFY(!view->viSelecting());
        QCOMPARE(pending->text(), "-- VI --");
        QTest::keyClicks(view, "V");
        QCOMPARE(pending->text(), "-- VISUAL LINE --");
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(view->viMode());
        QVERIFY(!view->viSelecting());
        QTest::keyClick(view, Qt::Key_Space, vimControl | Qt::ShiftModifier);
        QVERIFY(!view->viMode());
        QCOMPARE(pending->text(), "");
    }
    void searchWrapAndCancellation() {
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(m_pdf));
        QTest::qWait(40);
        auto *view = window.reader();
        view->search("attention");
        QTRY_COMPARE_WITH_TIMEOUT(view->matchCount(), 3, 4000);
        QCOMPARE(view->matchIndex(), 0);
        view->nextMatch();
        QCOMPARE(view->matchIndex(), 1);
        view->nextMatch(); view->nextMatch();
        QCOMPARE(view->matchIndex(), 0);
        view->nextMatch(true);
        QCOMPARE(view->matchIndex(), 2);
        view->clearSearch();
        QCOMPARE(view->matchCount(), 0);
        view->search("attention");
        view->search("nonexistent");
        QTest::qWait(200);
        QCOMPARE(view->matchCount(), 0);
        view->jump(0);
        view->search("attention", true);
        QTRY_COMPARE_WITH_TIMEOUT(view->matchCount(), 3, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(view->matchIndex() >= 0, 3000);
    }
    void visibleRastersSettle() {
        QPdfDocument document;
        QCOMPARE(document.load(m_pdf), QPdfDocument::Error::None);
        ReaderView view(&document);
        view.resize(2300, 1450);
        auto *renderer = view.findChild<QPdfPageRenderer *>();
        QVERIFY(renderer);
        QSignalSpy frames(renderer, &QPdfPageRenderer::pageRendered);
        view.show();
        view.setZoom(2.5);
        view.verticalScrollBar()->setValue(qRound(view.pageRect(0).bottom() - view.viewport()->height() / 2));
        // On Retina these two rasters used to evict each other from the 48MiB
        // cache forever. Once settled, idle viewing must schedule no more work.
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 2, 5000);
        QTest::qWait(800);
        const auto settled = frames.size();
        QTest::qWait(400);
        QCOMPARE(frames.size(), settled);
        view.viewport()->update();
        QTest::qWait(200);
        QCOMPARE(frames.size(), settled);
        view.setZoom(2.0);
        QTest::qWait(800);
        const auto resized = frames.size();
        QTest::qWait(400);
        QCOMPARE(frames.size(), resized);
    }
    void demoAndScreenshots() {
        const auto path = m_directory.filePath("Ivia field notes.pdf");
        QVERIFY(createDemo(path));
        QPdfDocument document;
        QCOMPARE(document.load(path), QPdfDocument::Error::None);
        QCOMPARE(document.pageCount(), 4);
        QVERIFY(document.getAllText(0).text().contains("Attention"));
        MainWindow window;
        window.resize(1180, 920);
        window.show();
        QTest::qWait(120);
        const auto output = qEnvironmentVariable("IVIA_SCREENSHOT_DIR");
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-welcome.png"));
        QVERIFY(window.openFile(path));
        window.reader()->setZoom(0.95);
        QVERIFY(window.executeCommand("cd " + m_directory.path()));
        QTest::qWait(700);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-reading.png"));
        QTest::keyClicks(window.reader(), ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QTest::keyClicks(prompt, "e ~/papers/");
        QTest::qWait(30);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-command.png"));
        QTest::keyClick(prompt, Qt::Key_Escape);
        QVERIFY(window.openFile(m_other));
        QVERIFY(window.executeCommand("b Ivia field notes.pdf"));
        QTest::qWait(50);
        QVERIFY(window.executeCommand("ls"));
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-buffers.png"));
        QTest::keyClick(window.centralWidget(), Qt::Key_Return);
        QTest::keyClicks(window.reader(), ":");
        QTest::keyClicks(prompt, "b");
        QTest::keyClick(prompt, Qt::Key_Tab);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-buffer-completion.png"));
        QTest::keyClick(prompt, Qt::Key_Escape);
        window.executeCommand("set dark");
        window.reader()->setZoom(0.9);
        QTest::qWait(300);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-dark.png"));
        QTest::keyClicks(window.reader(), ":");
        QTest::keyClicks(prompt, "e ~/papers/");
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-dark-command.png"));
        QTest::keyClick(prompt, Qt::Key_Escape);
    }
private:
    QTemporaryDir m_directory;
    QString m_pdf, m_other;
};

QTEST_MAIN(ReaderTest)
#include "reader_test.moc"
