#include "command.h"
#include "demo.h"
#include "main_window.h"
#include <QFile>
#include <QClipboard>
#include <QLineEdit>
#include <QLabel>
#include <QPdfDocument>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <memory>

using namespace ivia;

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
        QTest::keyClick(view, Qt::Key_D, Qt::MetaModifier);
        QVERIFY(view->verticalScrollBar()->value() > 100);
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QVERIFY(prompt->isVisible());
        QTest::keyClicks(prompt, "zoom 125");
        QTest::keyClick(prompt, Qt::Key_Return);
        QCOMPARE(view->zoom(), 1.25);
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
        QCOMPARE(view->size(), window.centralWidget()->size());
        view->scrollBy(0, 120);
        const auto viewportSize = view->viewport()->size();
        const auto pageOrigin = view->viewportPoint(0, {});
        QTest::keyClicks(view, ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QVERIFY(prompt->isVisible());
        QVERIFY(prompt->placeholderText().isEmpty());
        QCOMPARE(view->viewport()->size(), viewportSize);
        QCOMPARE(view->viewportPoint(0, {}), pageOrigin);
        QTest::keyClicks(prompt, "b ");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "b 1");
        QTest::keyClick(prompt, Qt::Key_Tab);
        QCOMPARE(prompt->text(), "b 2");
        QTest::keyClick(prompt, Qt::Key_Backtab);
        QCOMPARE(prompt->text(), "b 1");
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
        QVERIFY(window.executeCommand("bd"));
        QCOMPARE(window.bufferCount(), 0);
        QCOMPARE(window.reader(), nullptr);
    }
    void reloadIsTransactional() {
        const auto path = m_directory.filePath("Reload.pdf");
        QVERIFY(writeFixture(path));
        MainWindow window;
        window.show();
        QVERIFY(window.openFile(path));
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
        restored.show();
        restored.restoreSession();
        QTest::qWait(50);
        QCOMPARE(restored.bufferCount(), 1);
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
        view->copySelection();
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
        QTest::qWait(700);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-reading.png"));
        QTest::keyClicks(window.reader(), ":");
        auto *prompt = window.findChild<QLineEdit *>("commandLine");
        QTest::keyClicks(prompt, "e ~/papers/");
        QTest::qWait(30);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-command.png"));
        QTest::keyClick(prompt, Qt::Key_Escape);
        window.executeCommand("set dark");
        window.reader()->setZoom(0.9);
        QTest::qWait(300);
        if (!output.isEmpty()) QVERIFY(window.grab().save(output + "/ivia-dark.png"));
    }
private:
    QTemporaryDir m_directory;
    QString m_pdf, m_other;
};

QTEST_MAIN(ReaderTest)
#include "reader_test.moc"
