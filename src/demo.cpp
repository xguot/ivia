#include "demo.h"
#include <QFileInfo>
#include <QFont>
#include <QPainter>
#include <QPdfWriter>
#include <QTextDocument>

namespace ivia {
bool createDemo(const QString &path)
{
    QPdfWriter pdf(path);
    pdf.setTitle("The art of close reading");
    pdf.setCreator("Ivia");
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setResolution(72);
    QPainter painter(&pdf);
    if (!painter.isActive()) return false;
    const QStringList headings = {"The art of close reading", "Follow the thread", "Keep your place", "References & field notes"};
    const QStringList bodies = {
        R"(<p class="abstract"><b>ABSTRACT</b> &nbsp; A good reading environment gives an idea room to develop.
        This short field guide introduces a quieter way to work with academic papers: move deliberately,
        follow a reference, and find your way back without losing the thread.</p>
        <h2>1 &nbsp; Attention is a workspace</h2>
        <p>A paper is more than a sequence of pages. It is a small network of questions, evidence, and
        connections to other work. Reading well means moving through that network with intention.
        Sometimes we move linearly. Sometimes a single citation sends us somewhere new [1].</p>
        <p>Ivia is built around that rhythm. The page takes the center; the interface steps aside.
        A command line is there when you need it, and a handful of familiar movements carry you
        through everything else.</p>
        <h2>2 &nbsp; Learn a little, use it everywhere</h2>
        <p>Use <b>j</b> and <b>k</b> to move down and up. Add a number to make the movement larger:
        <b>5j</b> travels five steps. <b>Ctrl+D</b> moves half a screen down; <b>Ctrl+U</b> brings it back.
        The idea is fluency, rather than a long list of shortcuts.</p>
        <p>Type <b>gg</b> to return to the beginning, <b>G</b> to visit the final page, or <b>3G</b> to
        jump straight to page three. Zoom with <b>+</b> and <b>−</b>; <b>0</b> fits the page width.</p>
        <div class="note">TRY IT &nbsp; Type <b>/attention</b> and press Enter. Then use <b>n</b> to follow each occurrence.</div>)",
        R"(<h2>3 &nbsp; A paper is a conversation</h2>
        <p>References connect an argument to the work around it. Following one should feel like opening
        a parenthesis: a brief departure, with a dependable route back to where you started.</p>
        <p>In a linked PDF, press <b>f</b> to reveal labels over the visible links. Type a label to follow
        its destination. <b>F</b> opens an internal destination in a preview instead, keeping the original
        page in place. The same actions are available as <b>gd</b> and <b>gp</b> for the link beneath the pointer.</p>
        <p>For papers without embedded citation links, select a numbered citation such as <b>[1]</b>
        and type <b>gd</b>. Ivia looks for the corresponding numbered entry, scanning from the end of the
        document. You can also type <b>:ref 1</b>. This is a text heuristic, so unusual bibliographies
        and scanned papers may require a manual search.</p>
        <h2>4 &nbsp; The way back matters</h2>
        <p><b>Ctrl+O</b> returns to the place before a jump. <b>Ctrl+I</b> moves forward again. Page jumps,
        searches, links, and marks all share this history. Ordinary scrolling stays out of the way.</p>
        <p>Try <b>:ref 2</b>, inspect the entry, and press <b>Ctrl+O</b>. The attention you invested in
        the original paragraph is still there when you return.</p>
        <div class="note">A NOTE ON THIS GUIDE &nbsp; This is an original sample document, not a research publication.
        Its example references demonstrate the reader's navigation.</div>)",
        R"(<h2>5 &nbsp; Leave a small signpost</h2>
        <p>Not every useful place deserves a permanent annotation. Sometimes you only need to remember
        where a definition appeared, or where a particularly good question began.</p>
        <p>Type <b>ma</b> to set mark a. Later, <b>'a</b> returns to it. You have a full alphabet of marks,
        stored separately for every paper and remembered between sessions. Type <b>:marks</b> to see them.</p>
        <h2>6 &nbsp; A desk with more than one paper</h2>
        <p>Open another paper with <b>:e ~/papers/example.pdf</b>. Each document becomes a buffer,
        keeping its own position, search, and marks. Switch with <b>gt</b> and <b>gT</b>, or type
        <b>:ls</b> to see the list. <b>:b</b> followed by Tab lets you choose a relative filename.</p>
        <p>The command line completes names and PDF paths with Tab. Up and Down recall past commands.
        Close one buffer with <b>:bd</b>, or leave Ivia with <b>:q</b>. Your reading session will be ready
        the next time you open it.</p>
        <h2>7 &nbsp; Work at your own pace</h2>
        <p>A minimal reader should still be a practical tool. Drag to select a passage and press <b>y</b>
        to copy it. Open the embedded contents with <b>t</b>. Clear search highlights with <b>:noh</b>.
        If your typesetting tool has rebuilt the paper, <b>:reload</b> loads the updated file.</p>
        <div class="note">WHEN IN DOUBT &nbsp; Press <b>Esc</b> to return to normal mode. <b>:help</b> is the complete field guide.</div>)",
        R"(<h2>References</h2>
        <p>[1] Ivia project. Attention as a workspace. Example reference for the built-in reading guide, 2026.</p>
        <p>[2] Ivia project. Navigation, memory, and the way back. Example reference for testing citation jumps, 2026.</p>
        <h2>A few things to try</h2>
        <p>Search for <b>attention</b> with <b>/attention</b>. Use <b>n</b> to move forward and <b>N</b>
        to move back. Search is case-insensitive and includes every text-bearing page.</p>
        <p>Set a mark with <b>mr</b>. Jump to the beginning with <b>gg</b>, then type <b>'r</b> to return.</p>
        <p>Try <b>:zoom 125</b>, <b>:fit page</b>, and <b>:fit width</b>. Use <b>:set dark</b> for darker
        interface chrome, or <b>:set light</b> to return to the default. The paper keeps its original colors.</p>
        <p>Use <b>:2</b> to jump to the second page. Press <b>Ctrl+O</b> to return here.</p>
        <h2>Room for what comes next</h2>
        <p>This first edition concentrates on reading PDFs well. Annotation editing, OCR, synchronization,
        and author–year citation inference are future work. The document on disk is never modified.</p>
        <div class="note">THE ESSENTIALS &nbsp; <b>hjkl</b> to move. <b>/</b> to find. <b>:</b> to act.
        <b>gd</b> to follow. <b>Ctrl+O</b> to return.</div>)"
    };
    for (int page = 0; page < headings.size(); ++page) {
        if (page) pdf.newPage();
        painter.setPen(QColor("#666666"));
        painter.setFont(QFont("Helvetica Neue", 8));
        painter.drawText(QRectF(58, 42, 480, 16), "IVIA FIELD NOTES     /     01                                      READING, WITH INTENTION");
        painter.setPen(QColor("#dddddd"));
        painter.drawLine(QPointF(58, 69), QPointF(537, 69));
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setDefaultFont(QFont("Georgia", 11));
        document.setDefaultStyleSheet(R"(
            body {color:#000000;} h1{font-size:31pt;font-weight:normal;margin:0 0 12pt 0;}
            .byline{font-family:Helvetica;font-size:9pt;color:#666666;margin-bottom:28pt;}
            h2{font-family:Helvetica;font-size:12pt;margin-top:21pt;margin-bottom:10pt;color:#000000;}
            p{line-height:145%;margin-top:0;margin-bottom:13pt;}
            .abstract{font-size:10pt;color:#666666;line-height:145%;}
            .note{font-family:Helvetica;font-size:9pt;color:#000000;margin-top:22pt;line-height:150%;}
        )");
        document.setHtml("<h1>" + headings.at(page) + "</h1><p class='byline'>"
                         + (page == 0 ? "A practical guide to a quieter reading environment" : "The art of close reading · Ivia field notes")
                         + "</p>" + bodies.at(page));
        document.setTextWidth(479);
        painter.save();
        painter.translate(58, 99);
        document.drawContents(&painter, QRectF(0, 0, 479, 665));
        painter.restore();
        painter.setPen(QColor("#666666"));
        painter.setFont(QFont("Helvetica Neue", 8));
        painter.drawText(QRectF(58, 790, 479, 16), Qt::AlignLeft, "IVIA   /   A QUIET HOME FOR YOUR PAPERS");
        painter.drawText(QRectF(58, 790, 479, 16), Qt::AlignRight, QString::number(page + 1));
    }
    painter.end();
    return QFileInfo(path).size() > 0;
}
}
