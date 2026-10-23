#include "reader_view.h"
#include "theme.h"
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfLinkModel>
#include <QPdfPageRenderer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ivia {
namespace {
constexpr int margin = 40;
constexpr int gap = 24;
QString rasterKey(int page, QSize size)
{
    return QStringLiteral("%1:%2:%3").arg(page).arg(size.width()).arg(size.height());
}
}

ReaderView::ReaderView(QPdfDocument *document, QWidget *parent)
    : QAbstractScrollArea(parent), m_document(document),
      m_renderer(new QPdfPageRenderer(this))
{
    setObjectName("reader");
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    setMouseTracking(true);
    m_renderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    m_renderer->setDocument(document);
    connect(m_renderer, &QPdfPageRenderer::pageRendered, this,
            [this](int page, QSize size, const QImage &image,
                   QPdfDocumentRenderOptions, quint64) {
        const QString key = rasterKey(page, size);
        m_pending.remove(key);
        if (!image.isNull())
            m_cache.insert(key, new QImage(image), int(image.sizeInBytes() / 1024 + 1));
        viewport()->update();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_hinting) cancelHints();
        emit positionChanged();
    });
    m_searchTimer.setInterval(0);
    connect(&m_searchTimer, &QTimer::timeout, this, &ReaderView::searchStep);
    m_referenceTimer.setInterval(0);
    connect(&m_referenceTimer, &QTimer::timeout, this, &ReaderView::referenceStep);
    layoutPages();
}

ReaderView::~ReaderView()
{
    // Join the rendering worker before the owning buffer deletes its document.
    delete m_renderer;
}

QRectF ReaderView::pageRect(int page) const
{
    return page >= 0 && page < m_pages.size() ? m_pages.at(page) : QRectF{};
}

QPointF ReaderView::viewportPoint(int page, QPointF point) const
{
    return pageRect(page).topLeft() + point * m_zoom
            - QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

int ReaderView::currentPage() const
{
    const qreal y = verticalScrollBar()->value() + viewport()->height() * 0.28;
    auto it = std::lower_bound(m_pages.cbegin(), m_pages.cend(), y,
                              [](const QRectF &rect, qreal target) { return rect.bottom() < target; });
    return m_pages.isEmpty() ? 0 : std::min(int(it - m_pages.cbegin()), int(m_pages.size()) - 1);
}

Location ReaderView::location() const
{
    // The status page uses a point within the viewport; saved positions must
    // use its top edge so crossing a page boundary does not skip visible text.
    const qreal top = verticalScrollBar()->value();
    const auto it = std::lower_bound(m_pages.cbegin(), m_pages.cend(), top,
                                    [](const QRectF &rect, qreal y) { return rect.bottom() < y; });
    const int page = m_pages.isEmpty() ? 0 : std::min(int(it - m_pages.cbegin()), int(m_pages.size()) - 1);
    const auto rect = pageRect(page);
    return {page, QPointF(std::max(0.0, (horizontalScrollBar()->value() - rect.x()) / m_zoom),
                         std::max(0.0, (verticalScrollBar()->value() - rect.y()) / m_zoom)),
            m_zoom, m_fitWidth};
}

void ReaderView::layoutPages()
{
    if (m_layout) return;
    m_layout = true;
    qreal widest = 1;
    for (int i = 0; i < m_document->pageCount(); ++i)
        widest = std::max(widest, m_document->pagePointSize(i).width());
    if (m_fitWidth)
        m_zoom = std::clamp((viewport()->width() - 2.0 * margin) / widest, 0.1, 5.0);
    const qreal width = std::max(qreal(viewport()->width()), widest * m_zoom + 2 * margin);
    qreal y = gap;
    m_pages.clear();
    for (int i = 0; i < m_document->pageCount(); ++i) {
        const QSizeF size = m_document->pagePointSize(i) * m_zoom;
        m_pages.append(QRectF(QPointF((width - size.width()) / 2, y), size));
        y += size.height() + gap;
    }
    horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(width)) - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(y)) - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    m_layout = false;
    viewport()->update();
}

void ReaderView::restore(const Location &target)
{
    m_fitWidth = target.fitWidth;
    m_zoom = std::clamp(target.zoom, 0.1, 5.0);
    layoutPages();
    jump(target.page, target.point, false);
}

void ReaderView::jump(int page, QPointF point, bool record)
{
    if (m_pages.isEmpty()) return;
    page = std::clamp(page, 0, int(m_pages.size()) - 1);
    if (record) {
        m_back.append(location());
        if (m_back.size() > 200) m_back.removeFirst();
        m_forward.clear();
    }
    const QRectF rect = pageRect(page);
    verticalScrollBar()->setValue(qRound(rect.y() + point.y() * m_zoom));
    horizontalScrollBar()->setValue(qRound(rect.x() + point.x() * m_zoom - margin));
    emit positionChanged();
}

void ReaderView::back()
{
    if (m_back.isEmpty()) { emit message("At the oldest jump"); return; }
    m_forward.append(location());
    restore(m_back.takeLast());
}

void ReaderView::forward()
{
    if (m_forward.isEmpty()) { emit message("At the newest jump"); return; }
    m_back.append(location());
    restore(m_forward.takeLast());
}

void ReaderView::scrollBy(int x, int y)
{
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x);
    verticalScrollBar()->setValue(verticalScrollBar()->value() + y);
}

void ReaderView::setZoom(qreal factor)
{
    const Location before = location();
    m_fitWidth = false;
    m_zoom = std::clamp(factor, 0.1, 5.0);
    layoutPages();
    jump(before.page, before.point, false);
    emit positionChanged();
}

void ReaderView::fitWidth()
{
    const auto before = location();
    m_fitWidth = true;
    layoutPages();
    jump(before.page, before.point, false);
    emit positionChanged();
}

void ReaderView::fitPage()
{
    const auto size = m_document->pagePointSize(currentPage());
    if (size.isEmpty()) return;
    setZoom(std::min((viewport()->width() - margin * 2.0) / size.width(),
                     (viewport()->height() - gap * 2.0) / size.height()));
    jump(currentPage(), {}, false);
}

void ReaderView::setDark(bool dark)
{
    m_dark = dark;
    viewport()->update();
}

void ReaderView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const auto colors = theme(m_dark);
    painter.fillRect(viewport()->rect(), colors.canvas);
    const QPointF offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QRectF visible(offset, viewport()->size());
    painter.translate(-offset);
    for (int page = 0; page < m_pages.size(); ++page) {
        const auto rect = m_pages.at(page);
        if (!rect.intersects(visible)) continue;
        painter.fillRect(rect, Qt::white);
        qreal rasterScale = std::min(devicePixelRatioF(), 2.0);
        rasterScale = std::min(rasterScale, 6000.0 / std::max(rect.width(), rect.height()));
        rasterScale = std::min(rasterScale, std::sqrt(12e6 / (rect.width() * rect.height())));
        const QSize size = (rect.size() * rasterScale).toSize().expandedTo(QSize(1, 1));
        const auto key = rasterKey(page, size);
        if (const auto image = m_cache.object(key)) {
            painter.drawImage(rect, *image);
        } else if (!m_pending.contains(key) && m_pending.size() < 4) {
            m_pending.insert(key);
            m_renderer->requestPage(page, size);
        }
        painter.save();
        painter.translate(rect.topLeft());
        painter.scale(m_zoom, m_zoom);
        for (const auto &result : std::as_const(m_hits)) {
            if (result.page != page) continue;
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.searchHighlight);
            for (const auto &bound : result.bounds) painter.drawPolygon(bound);
        }
        if (m_match >= 0 && m_match < matchCount()) {
            const auto &result = m_hits.at(m_match);
            if (result.page == page) {
                painter.setPen(QPen(Qt::black, 1.0 / m_zoom));
                painter.setBrush(colors.activeSearchHighlight);
                for (const auto &bound : result.bounds) painter.drawPolygon(bound);
            }
        }
        if (m_selectionPage == page && m_selection) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.selectionHighlight);
            for (const auto &bound : m_selection->bounds()) painter.drawPolygon(bound);
        }
        painter.restore();
    }
    if (m_hinting) {
        painter.setFont(QFont("monospace", 11, QFont::DemiBold));
        for (const auto &hint : m_hints) {
            if (!hint.label.startsWith(m_hintPrefix)) continue;
            const auto origin = pageRect(hint.sourcePage).topLeft() + hint.rect.topLeft() * m_zoom;
            QRectF badge(origin, QSizeF(painter.fontMetrics().horizontalAdvance(hint.label) + 12, 23));
            painter.setPen(theme(false).border);
            painter.setBrush(theme(false).accentWash);
            painter.drawRect(badge);
            painter.setPen(theme(false).ink);
            painter.drawText(badge, Qt::AlignCenter, hint.label);
        }
    }
}

void ReaderView::resizeEvent(QResizeEvent *event)
{
    const auto before = location();
    QAbstractScrollArea::resizeEvent(event);
    layoutPages();
    jump(before.page, before.point, false);
}

void ReaderView::scrollContentsBy(int, int) { viewport()->update(); }

std::optional<ReaderView::Hit> ReaderView::hitTest(QPointF point) const
{
    point += QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int page = 0; page < m_pages.size(); ++page)
        if (m_pages.at(page).contains(point))
            return Hit{page, (point - m_pages.at(page).topLeft()) / m_zoom};
    return std::nullopt;
}

QPdfLink ReaderView::linkAt(const Hit &hit) const
{
    QPdfLinkModel links;
    links.setDocument(m_document);
    links.setPage(hit.page);
    return links.linkAt(hit.point);
}

void ReaderView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::BackButton) { back(); return; }
    if (event->button() == Qt::ForwardButton) { forward(); return; }
    if (event->button() != Qt::LeftButton) return;
    setFocus();
    cancelHints();
    clearSelection();
    m_press = hitTest(event->position());
    m_hover = m_press;
    m_dragging = false;
}

void ReaderView::mouseMoveEvent(QMouseEvent *event)
{
    m_hover = hitTest(event->position());
    if (m_press && (event->buttons() & Qt::LeftButton) && m_hover
            && m_press->page == m_hover->page) {
        if ((m_press->point - m_hover->point).manhattanLength() > 3) m_dragging = true;
        if (m_dragging) {
            m_selectionPage = m_press->page;
            m_selection = m_document->getSelection(m_selectionPage, m_press->point, m_hover->point);
            viewport()->update();
        }
    }
    viewport()->setCursor(m_hover && linkAt(*m_hover).isValid() ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void ReaderView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_press && !m_dragging) {
        const auto link = linkAt(*m_press);
        if (link.isValid()) activateLink(link, event->modifiers() & Qt::ShiftModifier);
    }
    m_press.reset();
}

void ReaderView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
        setZoom(m_zoom * (event->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void ReaderView::search(const QString &text, bool backwards)
{
    m_searchTimer.stop();
    m_hits.clear();
    m_searchText = text;
    m_searchPage = 0;
    m_searchBackwards = backwards;
    m_searchOrigin = location();
    m_initialMatchPending = !text.isEmpty();
    m_match = -1;
    if (!text.isEmpty()) m_searchTimer.start();
    viewport()->update();
    emit searchChanged();
}

QString ReaderView::searchText() const { return m_searchText; }
int ReaderView::matchCount() const { return m_hits.size(); }

void ReaderView::searchStep()
{
    // One page per event-loop turn: rendering and input remain responsive and a
    // replacement query can cancel the scan without stale results arriving.
    if (m_searchPage < m_document->pageCount()) {
        const int page = m_searchPage++;
        const auto text = m_document->getAllText(page).text();
        qsizetype offset = 0;
        while ((offset = text.indexOf(m_searchText, offset, Qt::CaseInsensitive)) >= 0) {
            const auto selection = m_document->getSelectionAtIndex(page, int(offset), m_searchText.size());
            m_hits.append({page, selection.boundingRectangle().topLeft(), selection.bounds()});
            offset += std::max(qsizetype(1), m_searchText.size());
            if (m_hits.size() >= 20000) {
                m_searchPage = m_document->pageCount();
                emit message("Showing the first 20,000 matches; narrow the search to see more");
                break;
            }
        }
    }
    if (m_searchPage >= m_document->pageCount()) m_searchTimer.stop();
    if (m_initialMatchPending) selectInitialMatch();
    if (!m_searchTimer.isActive() && m_hits.isEmpty()) emit message("Pattern not found: " + m_searchText);
    viewport()->update();
    emit searchChanged();
}

void ReaderView::selectInitialMatch()
{
    const int count = matchCount();
    if (!count) return;
    int candidate = -1;
    for (int i = 0; i < count; ++i) {
        const auto &result = m_hits.at(i);
        const bool before = result.page < m_searchOrigin.page
                || (result.page == m_searchOrigin.page && result.point.y() < m_searchOrigin.point.y());
        if (m_searchBackwards && before) candidate = i;
        else if (!m_searchBackwards && !before) { candidate = i; break; }
    }
    if (m_searchBackwards && m_searchPage <= m_searchOrigin.page && searching()) return;
    if (candidate < 0 && !searching()) candidate = m_searchBackwards ? count - 1 : 0;
    if (candidate >= 0) {
        m_initialMatchPending = false;
        selectMatch(candidate);
    }
}

void ReaderView::selectMatch(int index)
{
    if (index < 0 || index >= matchCount()) return;
    m_match = index;
    const auto &result = m_hits.at(index);
    jump(result.page, QPointF(0, std::max(0.0, result.point.y() - 90)));
    viewport()->update();
    emit searchChanged();
}

void ReaderView::nextMatch(bool reverse, int count)
{
    const int total = matchCount();
    if (!total) { emit message(searchText().isEmpty() ? "Use / to search" : "No matches yet"); return; }
    m_initialMatchPending = false;
    const int direction = (reverse != m_searchBackwards) ? -1 : 1;
    if (m_match < 0) selectMatch(direction < 0 ? total - 1 : 0);
    else selectMatch(((m_match + direction * (count % total)) % total + total) % total);
}

void ReaderView::clearSearch()
{
    m_initialMatchPending = false;
    m_searchTimer.stop();
    m_hits.clear();
    m_searchText.clear();
    m_match = -1;
    viewport()->update();
    emit searchChanged();
}

void ReaderView::beginHints(bool preview)
{
    m_hints.clear();
    m_hintPrefix.clear();
    m_hintPreview = preview;
    const QRectF visible(QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()), viewport()->size());
    for (int page = 0; page < m_pages.size(); ++page) {
        if (!m_pages.at(page).intersects(visible)) continue;
        QPdfLinkModel links;
        links.setDocument(m_document);
        links.setPage(page);
        for (int row = 0; row < links.rowCount({}); ++row) {
            auto link = links.data(links.index(row, 0), int(QPdfLinkModel::Role::Link)).value<QPdfLink>();
            const auto bounds = links.data(links.index(row, 0), int(QPdfLinkModel::Role::Rectangle)).toRectF();
            const QRectF onCanvas(m_pages.at(page).topLeft() + bounds.topLeft() * m_zoom,
                                  bounds.size() * m_zoom);
            if (onCanvas.intersects(visible) && link.isValid()) m_hints.append({{}, link, page, bounds});
        }
    }
    if (m_hints.isEmpty()) { emit message("No links in view. Select a citation and use gd, or :ref 12"); return; }
    const QString alphabet = "asdfghjklqwertyuiopzxcvbnm";
    int digits = 1;
    for (int capacity = alphabet.size(); capacity < m_hints.size(); capacity *= alphabet.size()) ++digits;
    for (int i = 0; i < m_hints.size(); ++i) {
        int value = i;
        QString label;
        for (int d = 0; d < digits; ++d) { label.prepend(alphabet.at(value % alphabet.size())); value /= alphabet.size(); }
        m_hints[i].label = label;
    }
    m_hinting = true;
    viewport()->update();
    emit hintsChanged(true);
    emit message(preview ? "Type a link label to preview · Esc cancels" : "Type a link label to follow · Esc cancels");
}

bool ReaderView::hintKey(const QString &text)
{
    if (!m_hinting) return false;
    m_hintPrefix += text.toLower();
    bool prefixExists = false;
    for (const auto &hint : std::as_const(m_hints)) {
        if (hint.label == m_hintPrefix) {
            const auto link = hint.link;
            const bool preview = m_hintPreview;
            cancelHints();
            activateLink(link, preview);
            return true;
        }
        prefixExists |= hint.label.startsWith(m_hintPrefix);
    }
    if (!prefixExists) { cancelHints(); emit message("No such link label"); }
    viewport()->update();
    return true;
}

void ReaderView::cancelHints()
{
    if (!m_hinting) return;
    m_hinting = false;
    m_hints.clear();
    m_hintPrefix.clear();
    viewport()->update();
    emit hintsChanged(false);
}

void ReaderView::activateLink(const QPdfLink &link, bool preview)
{
    if (link.page() >= 0) {
        if (preview) emit previewRequested(link.page(), link.location());
        else jump(link.page(), link.location());
    } else if (!link.url().isEmpty()) {
        if (preview) emit message(link.url().toDisplayString());
        else emit externalLinkRequested(link.url());
    }
}

void ReaderView::followReference(bool preview)
{
    if (m_hover) {
        const auto link = linkAt(*m_hover);
        if (link.isValid()) { activateLink(link, preview); return; }
    }
    QString citation = selectedText().trimmed();
    if (citation.isEmpty() && m_hover) {
        // Extract a short run around the pointer. Restrict fallback matching to
        // numbered citations rather than guessing an author name.
        const auto hit = *m_hover;
        citation = m_document->getSelection(hit.page, hit.point - QPointF(20, 0),
                                            hit.point + QPointF(25, 0)).text().trimmed();
        const auto match = QRegularExpression(R"(\[(\d+)\])").match(citation);
        citation = match.hasMatch() ? match.captured(1) : QString{};
    }
    if (!citation.isEmpty() && !preview) { findReference(citation); return; }
    beginHints(preview);
}

void ReaderView::findReference(QString citation)
{
    citation = citation.trimmed();
    const auto number = QRegularExpression(R"(^\[?(\d+)\]?$)").match(citation);
    if (!number.hasMatch()) { emit message("Select one numbered citation, or use :ref 12"); return; }
    m_referencePattern = QRegularExpression(
        QStringLiteral(R"((?:^|[\r\n])\s*(?:\[%1\]|%1[.)])\s+)")
            .arg(QRegularExpression::escape(number.captured(1))));
    m_referencePage = m_document->pageCount() - 1;
    m_referenceTimer.start();
    emit message("Looking for reference [" + number.captured(1) + "]…");
}

void ReaderView::referenceStep()
{
    if (m_referencePage < 0) {
        m_referenceTimer.stop();
        emit message("Reference not found. This PDF may be scanned or use author–year citations.");
        return;
    }
    const int page = m_referencePage--;
    const auto text = m_document->getAllText(page).text();
    const auto match = m_referencePattern.match(text);
    if (!match.hasMatch()) return;
    m_referenceTimer.stop();
    const auto selection = m_document->getSelectionAtIndex(page, int(match.capturedStart()), int(match.capturedLength()));
    jump(page, QPointF(0, std::max(0.0, selection.boundingRectangle().top() - 70)));
    emit message("Reference found · Ctrl+O returns");
}

void ReaderView::copySelection()
{
    if (!m_selection || !m_selection->isValid()) { emit message("Drag to select text first"); return; }
    m_selection->copyToClipboard();
    emit message("Copied selection");
}

void ReaderView::clearSelection()
{
    m_selection.reset();
    m_selectionPage = -1;
    viewport()->update();
}
}
