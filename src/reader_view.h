#pragma once
#include <QAbstractScrollArea>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QPdfLink>
#include <QPdfSelection>
#include <QSet>
#include <QRegularExpression>
#include <QTimer>
#include <optional>

class QPdfDocument;
class QPdfPageRenderer;

namespace ivia {
struct Location {
    int page = 0;
    QPointF point;
    qreal zoom = 1.0;
    bool fitWidth = true;
};

class ReaderView final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ReaderView(QPdfDocument *document, QWidget *parent = nullptr);
    ~ReaderView() override;
    QPdfDocument *document() const { return m_document; }
    int currentPage() const;
    qreal zoom() const { return m_zoom; }
    bool fitsWidth() const { return m_fitWidth; }
    Location location() const;
    void restore(const Location &location);
    void jump(int page, QPointF point = {}, bool record = true);
    void back();
    void forward();
    void scrollBy(int x, int y);
    void setZoom(qreal zoom);
    void fitWidth();
    void fitPage();
    void setDark(bool dark);
    void search(const QString &text, bool backwards = false);
    void nextMatch(bool reverse = false, int count = 1);
    void clearSearch();
    QString searchText() const;
    int matchCount() const;
    int matchIndex() const { return m_match; }
    bool searching() const { return m_searchTimer.isActive(); }
    void beginHints(bool preview = false);
    bool hintKey(const QString &text);
    bool hinting() const { return m_hinting; }
    void cancelHints();
    void toggleViMode();
    void exitViMode();
    bool viMode() const { return m_viMode; }
    bool viSelecting() const { return m_viSelect != ViSelect::None; }
    QString viModeLabel() const;
    bool handleViKey(int key, Qt::KeyboardModifiers modifiers, const QString &text, int count = 1, const QString &pending = {});
    void followReference(bool preview = false);
    void findReference(QString citation);
    void copySelection();
    void clearSelection();
    QString selectedText() const { return m_selection ? m_selection->text() : QString{}; }
    QRectF pageRect(int page) const;
    QPointF viewportPoint(int page, QPointF point) const;

signals:
    void positionChanged();
    void searchChanged();
    void message(const QString &text);
    void hintsChanged(bool active);
    void viModeChanged(bool active);
    void previewRequested(int page, QPointF point);
    void externalLinkRequested(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void scrollContentsBy(int, int) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    enum class ViSelect { None, Normal, Line };
    struct Hit { int page; QPointF point; };
    struct Hint { QString label; QPdfLink link; int sourcePage; QRectF rect; };
    struct SearchHit { int page; QPointF point; QList<QPolygonF> bounds; };
    std::optional<Hit> hitTest(QPointF point) const;
    QPdfLink linkAt(const Hit &hit) const;
    void activateLink(const QPdfLink &link, bool preview);
    void layoutPages();
    void requestVisibleRasters();
    void selectMatch(int index);
    void selectInitialMatch();
    void searchStep();
    void referenceStep();
    void ensurePageText(int page);
    QRectF viCharRect(int page, int index) const;
    void updateViSelection();
    void ensureViCursorVisible();
    void moveViVertical(int deltaLines);

    QPdfDocument *m_document;
    QPdfPageRenderer *m_renderer;
    QVector<QRectF> m_pages;
    QCache<QString, QImage> m_cache{48 * 1024}; // KiB; per-document bounded raster cache.
    QCache<int, QImage> m_previousRasters{12 * 1024}; // Small previews survive zoom/resize.
    QHash<int, QSize> m_rasterTargets;
    QSet<QString> m_pending;
    QSet<QString> m_failedRasters;
    qreal m_zoom = 1;
    bool m_fitWidth = true;
    bool m_dark = false;
    bool m_layout = false;
    QList<Location> m_back, m_forward;
    int m_match = -1;
    bool m_searchBackwards = false;
    bool m_initialMatchPending = false;
    Location m_searchOrigin;
    QString m_searchText;
    QList<SearchHit> m_hits;
    QTimer m_searchTimer;
    int m_searchPage = 0;
    std::optional<QPdfSelection> m_selection;
    int m_selectionPage = -1;
    std::optional<Hit> m_press, m_hover;
    bool m_dragging = false;
    bool m_hinting = false;
    bool m_hintPreview = false;
    QString m_hintPrefix;
    QList<Hint> m_hints;
    bool m_viMode = false;
    ViSelect m_viSelect = ViSelect::None;
    int m_viPage = 0;
    int m_viIndex = 0;
    int m_viAnchor = -1;
    QString m_viText;
    int m_viTextPage = -1;
    QVector<QPair<int, int>> m_viLines;
    QTimer m_referenceTimer;
    QRegularExpression m_referencePattern;
    int m_referencePage = -1;
};
}
