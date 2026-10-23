#include "reader_view.h"
#include "command_line.h"
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
// All visible full-page rasters must coexist in the cache. A per-page cap is
// insufficient: two large pages would otherwise evict and rerender each other.
constexpr qreal visibleRasterBytes = 36.0 * 1024 * 1024;
constexpr qreal previousRasterPixels = 512.0 * 1024;
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
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setMouseTracking(true);
    setMouseTracking(true);
    m_renderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    m_renderer->setDocument(document);
    connect(m_renderer, &QPdfPageRenderer::pageRendered, this,
            [this](int page, QSize size, const QImage &image,
                   QPdfDocumentRenderOptions, quint64) {
        const QString key = rasterKey(page, size);
        m_pending.remove(key);
        // An old zoom/viewport request must not evict the current view's images.
        if (m_rasterTargets.value(page) == size) {
            if (image.isNull()) {
                // A corrupt page must not trigger an endless repaint/retry loop.
                m_failedRasters.insert(key);
            } else {
                m_cache.insert(key, new QImage(image), int(image.sizeInBytes() / 1024 + 1));
                const qreal previewScale = std::min({1.0,
                    2048.0 / std::max(image.width(), image.height()),
                    std::sqrt(previousRasterPixels / (qreal(image.width()) * image.height()))});
                const auto previewSize = QSize(std::max(1, int(image.width() * previewScale)),
                                               std::max(1, int(image.height() * previewScale)));
                auto *preview = new QImage(image.scaled(previewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                m_previousRasters.insert(page, preview, int(preview->sizeInBytes() / 1024 + 1));
                const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
                viewport()->update(pageRect(page).translated(-offset).toAlignedRect());
            }
        }
        // Obsolete requests still free worker capacity. Start the latest visible
        // work immediately without repainting solely for a discarded result.
        requestVisibleRasters();
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

void ReaderView::requestVisibleRasters()
{
    for (auto target = m_rasterTargets.cbegin(); target != m_rasterTargets.cend(); ++target) {
        if (m_pending.size() >= 4) break;
        const auto key = rasterKey(target.key(), target.value());
        if (m_cache.contains(key) || m_pending.contains(key) || m_failedRasters.contains(key)) continue;
        m_pending.insert(key);
        m_renderer->requestPage(target.key(), target.value());
    }
}

void ReaderView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const auto colors = theme(m_dark);
    painter.fillRect(viewport()->rect(), colors.canvas);
    const QPointF offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QRectF visible(offset, viewport()->size());
    QHash<int, QSize> targets;
    qreal totalPixels = 0;
    for (int page = 0; page < m_pages.size(); ++page) {
        const auto rect = m_pages.at(page);
        if (!rect.intersects(visible) || rect.isEmpty()) continue;
        qreal scale = std::min(devicePixelRatioF(), 2.0);
        scale = std::min(scale, 6000.0 / std::max(rect.width(), rect.height()));
        scale = std::min(scale, std::sqrt(12e6 / (rect.width() * rect.height())));
        const QSize size = (rect.size() * scale).toSize().expandedTo(QSize(1, 1));
        targets.insert(page, size);
        totalPixels += qreal(size.width()) * size.height();
    }
    // Leave room for cache accounting and previously visited pages. Round down
    // so the aggregate remains within this budget at every device pixel ratio.
    const qreal budgetPixels = std::max(1.0, (visibleRasterBytes - targets.size() * 1024.0) / 4.0);
    const qreal sharedScale = totalPixels > budgetPixels ? std::sqrt(budgetPixels / totalPixels) : 1.0;
    QSet<QString> activeKeys;
    for (auto target = targets.begin(); target != targets.end(); ++target) {
        target.value() = QSize(std::max(1, int(target->width() * sharedScale)),
                               std::max(1, int(target->height() * sharedScale)));
        activeKeys.insert(rasterKey(target.key(), target.value()));
    }
    m_rasterTargets = std::move(targets);
    m_failedRasters.intersect(activeKeys);
    painter.translate(-offset);
    for (int page = 0; page < m_pages.size(); ++page) {
        const auto rect = m_pages.at(page);
        if (!rect.intersects(visible)) continue;
        painter.fillRect(rect, Qt::white);
        const auto key = rasterKey(page, m_rasterTargets.value(page));
        if (const auto image = m_cache.object(key)) {
            painter.drawImage(rect, *image);
        } else if (const auto previous = m_previousRasters.object(page)) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(rect, *previous);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
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
        if (m_viMode && m_viPage == page) {
            const QRectF caret = viCharRect(page, m_viIndex);
            if (!caret.isEmpty()) {
                painter.setPen(QPen(Qt::black, 1.0 / m_zoom));
                painter.setBrush(QColor(0, 0, 0, 115));
                painter.drawRect(caret);
            }
        }
        painter.restore();
    }
    requestVisibleRasters();
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
    if (m_viMode) exitViMode();
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
    if (m_viMode) {
        ensurePageText(result.page);
        if (!m_viText.isEmpty() && !m_searchText.isEmpty()) {
            qsizetype pos = 0;
            int best = m_viIndex;
            qreal bestDist = 1e9;
            while ((pos = m_viText.indexOf(m_searchText, pos, Qt::CaseInsensitive)) >= 0) {
                const auto rect = viCharRect(result.page, int(pos));
                const qreal dist = (rect.topLeft() - result.point).manhattanLength();
                if (dist < bestDist) { bestDist = dist; best = int(pos); }
                pos += std::max(qsizetype(1), m_searchText.size());
            }
            m_viPage = result.page;
            m_viIndex = best;
            updateViSelection();
        }
    }
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

void ReaderView::ensurePageText(int page)
{
    page = std::clamp(page, 0, std::max(0, m_document->pageCount() - 1));
    if (m_viTextPage == page) return;
    m_viTextPage = page;
    m_viText = m_document->getAllText(page).text();
    m_viLines.clear();
    int start = 0;
    for (int i = 0; i < m_viText.size(); ++i) {
        if (m_viText.at(i) == '\n') {
            int end = i;
            if (end > start && m_viText.at(end - 1) == '\r') --end;
            m_viLines.append({start, std::max(start, end - 1)});
            start = i + 1;
        }
    }
    if (start < m_viText.size() || m_viLines.isEmpty()) {
        int end = int(m_viText.size()) - 1;
        if (end >= start && m_viText.at(end) == '\r') --end;
        m_viLines.append({start, std::max(start, end)});
    }
}

QRectF ReaderView::viCharRect(int page, int index) const
{
    if (page < 0 || page >= m_document->pageCount()) return {};
    const auto text = page == m_viTextPage ? m_viText : m_document->getAllText(page).text();
    if (text.isEmpty()) return QRectF(72, 72, 8, 14);
    index = std::clamp(index, 0, int(text.size()) - 1);
    for (int step = 0; step < 32; ++step) {
        for (int sign : {1, -1}) {
            const int candidate = index + step * sign;
            if (candidate < 0 || candidate >= text.size()) continue;
            if (text.at(candidate).isSpace()) continue;
            const auto sel = m_document->getSelectionAtIndex(page, candidate, 1);
            const auto rect = sel.boundingRectangle();
            if (!rect.isEmpty()) {
                if (candidate == index) return rect;
                const qreal width = std::clamp(rect.width(), 5.0, 12.0);
                return QRectF(rect.x() + (index - candidate) * width * 0.6, rect.y(), width, rect.height());
            }
        }
    }
    return QRectF(72, 72, 8, 14);
}

void ReaderView::updateViSelection()
{
    if (!m_viMode || m_viSelect == ViSelect::None || m_viAnchor < 0) {
        m_selection.reset();
        m_selectionPage = -1;
        viewport()->update();
        return;
    }
    ensurePageText(m_viPage);
    if (m_viText.isEmpty()) return;
    int first = std::min(m_viAnchor, m_viIndex);
    int last = std::max(m_viAnchor, m_viIndex);
    if (m_viSelect == ViSelect::Line) {
        for (const auto &line : std::as_const(m_viLines)) {
            if (first >= line.first && first <= line.second + 1) first = line.first;
            if (last >= line.first && last <= line.second + 1) last = line.second;
        }
    }
    first = std::clamp(first, 0, int(m_viText.size()) - 1);
    last = std::clamp(last, first, int(m_viText.size()) - 1);
    m_selectionPage = m_viPage;
    m_selection = m_document->getSelectionAtIndex(m_viPage, first, last - first + 1);
    viewport()->update();
}

void ReaderView::ensureViCursorVisible()
{
    if (!m_viMode || m_pages.isEmpty()) return;
    const QRectF page = pageRect(m_viPage);
    const QRectF caret = viCharRect(m_viPage, m_viIndex);
    const QRectF onCanvas(page.topLeft() + caret.topLeft() * m_zoom, caret.size() * m_zoom);
    const qreal top = verticalScrollBar()->value();
    const qreal bottom = top + viewport()->height();
    const qreal pad = 36.0;
    if (onCanvas.top() < top + pad)
        verticalScrollBar()->setValue(qRound(std::max(0.0, onCanvas.top() - pad)));
    else if (onCanvas.bottom() > bottom - pad)
        verticalScrollBar()->setValue(qRound(onCanvas.bottom() - viewport()->height() + pad));
    viewport()->update();
    emit positionChanged();
}

void ReaderView::moveViVertical(int deltaLines)
{
    ensurePageText(m_viPage);
    int lineIdx = 0;
    for (int i = 0; i < m_viLines.size(); ++i) {
        if (m_viIndex <= m_viLines.at(i).second + 1) { lineIdx = i; break; }
        lineIdx = i;
    }
    const int column = m_viLines.isEmpty() ? 0 : std::max(0, m_viIndex - m_viLines.at(lineIdx).first);
    int targetLine = lineIdx + deltaLines;
    while (targetLine < 0 && m_viPage > 0) {
        --m_viPage;
        ensurePageText(m_viPage);
        targetLine += std::max(1, int(m_viLines.size()));
    }
    while (targetLine >= m_viLines.size() && m_viPage + 1 < m_document->pageCount()) {
        targetLine -= std::max(1, int(m_viLines.size()));
        ++m_viPage;
        ensurePageText(m_viPage);
    }
    if (m_viLines.isEmpty()) { m_viIndex = 0; return; }
    targetLine = std::clamp(targetLine, 0, int(m_viLines.size()) - 1);
    const auto &line = m_viLines.at(targetLine);
    m_viIndex = std::min(line.first + column, line.second);
}

void ReaderView::toggleViMode()
{
    if (m_viMode) { exitViMode(); return; }
    cancelHints();
    m_viMode = true;
    m_viSelect = ViSelect::None;
    m_viAnchor = -1;
    m_viPage = currentPage();
    ensurePageText(m_viPage);
    m_viIndex = 0;
    const qreal targetY = (verticalScrollBar()->value() - pageRect(m_viPage).y()) / m_zoom + 24.0;
    for (const auto &line : std::as_const(m_viLines)) {
        const auto rect = viCharRect(m_viPage, line.first);
        if (rect.bottom() >= std::max(0.0, targetY)) { m_viIndex = line.first; break; }
    }
    ensureViCursorVisible();
    viewport()->update();
    emit viModeChanged(true);
}

void ReaderView::exitViMode()
{
    if (!m_viMode) return;
    m_viMode = false;
    m_viSelect = ViSelect::None;
    m_viAnchor = -1;
    clearSelection();
    viewport()->update();
    emit viModeChanged(false);
}

QString ReaderView::viModeLabel() const
{
    if (!m_viMode) return {};
    if (m_viSelect == ViSelect::Line) return "-- VISUAL LINE --";
    if (m_viSelect == ViSelect::Normal) return "-- VISUAL --";
    return "-- VI --";
}

bool ReaderView::handleViKey(int key, Qt::KeyboardModifiers modifiers, const QString &text, int count, const QString &pending)
{
    if (!m_viMode) return false;
    ensurePageText(m_viPage);
    auto currentLineIndex = [&]() {
        for (int i = 0; i < m_viLines.size(); ++i)
            if (m_viIndex <= m_viLines.at(i).second + 1) return i;
        return std::max(0, int(m_viLines.size()) - 1);
    };
    auto isWord = [](QChar ch) { return ch.isLetterOrNumber() || ch.isMark() || ch == '_'; };

    if (key == Qt::Key_Escape) {
        if (m_viSelect != ViSelect::None) {
            m_viSelect = ViSelect::None;
            m_viAnchor = -1;
            clearSelection();
            emit viModeChanged(true);
        } else {
            exitViMode();
        }
        return true;
    }
    if (CommandLine::physicalControl(modifiers)) {
        if (key == Qt::Key_C) { exitViMode(); return true; }
        if (key == Qt::Key_E) { scrollBy(0, 24 * count); return true; }
        if (key == Qt::Key_Y) { scrollBy(0, -24 * count); return true; }
        if (key == Qt::Key_D) { moveViVertical(10 * count); ensureViCursorVisible(); updateViSelection(); return true; }
        if (key == Qt::Key_U) { moveViVertical(-10 * count); ensureViCursorVisible(); updateViSelection(); return true; }
        if (key == Qt::Key_F) { moveViVertical(20 * count); ensureViCursorVisible(); updateViSelection(); return true; }
        if (key == Qt::Key_B) { moveViVertical(-20 * count); ensureViCursorVisible(); updateViSelection(); return true; }
        return false;
    }
    if (!pending.isEmpty()) {
        const QString seq = pending + text;
        if (seq == "gg") {
            m_viPage = count > 1 ? std::clamp(count - 1, 0, m_document->pageCount() - 1) : 0;
            ensurePageText(m_viPage);
            m_viIndex = 0;
            ensureViCursorVisible();
            updateViSelection();
            return true;
        }
        if (seq == "gd" || seq == "gp") {
            followReference(seq == "gp");
            return true;
        }
        if (seq == "zz") {
            const auto rect = viCharRect(m_viPage, m_viIndex);
            verticalScrollBar()->setValue(qRound(pageRect(m_viPage).y() + rect.center().y() * m_zoom - viewport()->height() / 2.0));
            return true;
        }
        return false;
    }

    if (key == Qt::Key_Left || text == "h") {
        m_viIndex = std::max(0, m_viIndex - count);
    } else if (key == Qt::Key_Right || text == "l") {
        m_viIndex = std::min(std::max(0, int(m_viText.size()) - 1), m_viIndex + count);
    } else if (key == Qt::Key_Down || text == "j") {
        moveViVertical(count);
    } else if (key == Qt::Key_Up || text == "k") {
        moveViVertical(-count);
    } else if (key == Qt::Key_Home || text == "0") {
        if (!m_viLines.isEmpty()) m_viIndex = m_viLines.at(currentLineIndex()).first;
    } else if (text == "^") {
        if (!m_viLines.isEmpty()) {
            const auto &line = m_viLines.at(currentLineIndex());
            int pos = line.first;
            while (pos < line.second && m_viText.at(pos).isSpace()) ++pos;
            m_viIndex = pos;
        }
    } else if (key == Qt::Key_End || text == "$") {
        if (!m_viLines.isEmpty()) m_viIndex = m_viLines.at(currentLineIndex()).second;
    } else if (text == "w" || text == "W") {
        const bool big = text == "W";
        for (int c = 0; c < count; ++c) {
            const int n = m_viText.size();
            if (m_viIndex >= n - 1) break;
            if (!big && !m_viText.at(m_viIndex).isSpace()) {
                const bool w = isWord(m_viText.at(m_viIndex));
                while (m_viIndex < n - 1 && !m_viText.at(m_viIndex).isSpace() && isWord(m_viText.at(m_viIndex)) == w) ++m_viIndex;
            } else if (big) {
                while (m_viIndex < n - 1 && !m_viText.at(m_viIndex).isSpace()) ++m_viIndex;
            }
            while (m_viIndex < n - 1 && m_viText.at(m_viIndex).isSpace()) ++m_viIndex;
        }
    } else if (text == "b" || text == "B") {
        const bool big = text == "B";
        for (int c = 0; c < count; ++c) {
            if (m_viIndex <= 0) break;
            --m_viIndex;
            while (m_viIndex > 0 && m_viText.at(m_viIndex).isSpace()) --m_viIndex;
            if (!big) {
                const bool w = isWord(m_viText.at(m_viIndex));
                while (m_viIndex > 0 && !m_viText.at(m_viIndex - 1).isSpace() && isWord(m_viText.at(m_viIndex - 1)) == w) --m_viIndex;
            } else {
                while (m_viIndex > 0 && !m_viText.at(m_viIndex - 1).isSpace()) --m_viIndex;
            }
        }
    } else if (text == "e" || text == "E") {
        const bool big = text == "E";
        for (int c = 0; c < count; ++c) {
            const int n = m_viText.size();
            if (m_viIndex >= n - 1) break;
            ++m_viIndex;
            while (m_viIndex < n - 1 && m_viText.at(m_viIndex).isSpace()) ++m_viIndex;
            if (!big) {
                const bool w = isWord(m_viText.at(m_viIndex));
                while (m_viIndex < n - 1 && !m_viText.at(m_viIndex + 1).isSpace() && isWord(m_viText.at(m_viIndex + 1)) == w) ++m_viIndex;
            } else {
                while (m_viIndex < n - 1 && !m_viText.at(m_viIndex + 1).isSpace()) ++m_viIndex;
            }
        }
    } else if (text == "H") {
        if (!m_viLines.isEmpty()) m_viIndex = m_viLines.first().first;
    } else if (text == "M") {
        if (!m_viLines.isEmpty()) m_viIndex = m_viLines.at(m_viLines.size() / 2).first;
    } else if (text == "L") {
        if (!m_viLines.isEmpty()) m_viIndex = m_viLines.last().first;
    } else if (text == "G") {
        m_viPage = count > 1 ? std::clamp(count - 1, 0, m_document->pageCount() - 1) : std::max(0, m_document->pageCount() - 1);
        ensurePageText(m_viPage);
        m_viIndex = m_viLines.isEmpty() ? 0 : m_viLines.last().first;
    } else if (text == "v") {
        if (m_viSelect == ViSelect::Normal) {
            m_viSelect = ViSelect::None;
            m_viAnchor = -1;
            clearSelection();
        } else {
            if (m_viSelect == ViSelect::None) m_viAnchor = m_viIndex;
            m_viSelect = ViSelect::Normal;
            updateViSelection();
        }
        emit viModeChanged(true);
        return true;
    } else if (text == "V") {
        if (m_viSelect == ViSelect::Line) {
            m_viSelect = ViSelect::None;
            m_viAnchor = -1;
            clearSelection();
        } else {
            if (m_viSelect == ViSelect::None) m_viAnchor = m_viIndex;
            m_viSelect = ViSelect::Line;
            updateViSelection();
        }
        emit viModeChanged(true);
        return true;
    } else if (text == "y" || text == "Y") {
        if (text == "Y" && m_viSelect == ViSelect::None && !m_viLines.isEmpty()) {
            const auto &line = m_viLines.at(currentLineIndex());
            m_selectionPage = m_viPage;
            m_selection = m_document->getSelectionAtIndex(m_viPage, line.first, line.second - line.first + 1);
        } else if (m_viSelect == ViSelect::None && !m_selection && !m_viLines.isEmpty()) {
            const auto &line = m_viLines.at(currentLineIndex());
            m_selectionPage = m_viPage;
            m_selection = m_document->getSelectionAtIndex(m_viPage, line.first, line.second - line.first + 1);
        }
        copySelection();
        m_viSelect = ViSelect::None;
        m_viAnchor = -1;
        clearSelection();
        emit viModeChanged(true);
        return true;
    } else if (text == "i") {
        exitViMode();
        return true;
    } else if (text == "z") {
        const auto rect = viCharRect(m_viPage, m_viIndex);
        verticalScrollBar()->setValue(qRound(pageRect(m_viPage).y() + rect.center().y() * m_zoom - viewport()->height() / 2.0));
        return true;
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        followReference(false);
        return true;
    } else {
        return false;
    }

    ensureViCursorVisible();
    updateViSelection();
    return true;
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
    if (m_viMode) {
        const auto caret = viCharRect(m_viPage, m_viIndex);
        const auto link = linkAt({m_viPage, caret.center()});
        if (link.isValid()) { activateLink(link, preview); return; }
        QString citation = selectedText().trimmed();
        if (citation.isEmpty()) {
            ensurePageText(m_viPage);
            const int start = std::max(0, m_viIndex - 12);
            const int len = std::min(int(m_viText.size()) - start, 28);
            const auto slice = m_viText.mid(start, len);
            const auto match = QRegularExpression(R"(\[(\d+)\])").match(slice);
            if (match.hasMatch()) citation = match.captured(1);
        }
        if (!citation.isEmpty() && !preview) { findReference(citation); return; }
    }
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
