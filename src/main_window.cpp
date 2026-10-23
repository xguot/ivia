#include "main_window.h"
#include "command.h"
#include "command_line.h"
#include "theme.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMimeData>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <functional>

namespace ivia {
namespace {
QJsonObject encodeLocation(const Location &location)
{
    return {{"page", location.page}, {"x", location.point.x()}, {"y", location.point.y()},
            {"zoom", location.zoom}, {"fit", location.fitWidth}};
}
Location decodeLocation(const QJsonObject &object)
{
    return {object["page"].toInt(), QPointF(object["x"].toDouble(), object["y"].toDouble()),
            object["zoom"].toDouble(1), object["fit"].toBool(true)};
}
QString stateKey(const QString &path)
{
    return "documents/" + QString::fromLatin1(path.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
}
QPushButton *button(const QString &text, QWidget *parent = nullptr)
{
    auto *result = new QPushButton(text, parent);
    result->setCursor(Qt::PointingHandCursor);
    result->setFocusPolicy(Qt::NoFocus);
    return result;
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setObjectName("ivia");
    setWindowTitle("Ivia");
    resize(1180, 860);
    setMinimumSize(640, 460);
    setAcceptDrops(true);
    m_dark = QSettings().value("appearance/dark", false).toBool();
    buildUi();
    applyTheme();
    qApp->installEventFilter(this);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, m_messageLabel, &QWidget::hide);
    m_saveTimer.setInterval(5000);
    connect(&m_saveTimer, &QTimer::timeout, this, &MainWindow::saveSession);
    m_saveTimer.start();
    const auto geometry = QSettings().value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    updateStatus();
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    m_active = -1;
    // Removing the active stacked widget can resize another buffer. Stop its
    // notifications before any view is destroyed, so teardown cannot inspect
    // a buffer whose view has already been deleted.
    for (const auto &item : m_buffers) item->view->disconnect(this);
    for (auto &item : m_buffers) {
        delete item->view;
        delete item->document;
    }
}

void MainWindow::buildUi()
{
    auto *central = new QWidget;
    central->setObjectName("canvas");
    central->setFocusPolicy(Qt::StrongFocus);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setCentralWidget(central);

    auto *splitter = new QSplitter;
    splitter->setHandleWidth(1);
    m_panel = new QWidget;
    m_panel->setObjectName("panel");
    m_panel->setMinimumWidth(220);
    auto *panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(16, 18, 12, 10);
    auto *panelHeading = new QHBoxLayout;
    m_panelTitle = new QLabel;
    m_panelTitle->setObjectName("panelTitle");
    panelHeading->addWidget(m_panelTitle);
    panelHeading->addStretch();
    auto *close = button("×");
    close->setFixedWidth(28);
    connect(close, &QPushButton::clicked, this, [this] { m_panel->hide(); if (reader()) reader()->setFocus(); });
    panelHeading->addWidget(close);
    panelLayout->addLayout(panelHeading);
    m_panelTree = new QTreeWidget;
    m_panelTree->setObjectName("panelTree");
    m_panelTree->setHeaderHidden(true);
    m_panelTree->setIndentation(14);
    m_panelTree->setUniformRowHeights(true);
    panelLayout->addWidget(m_panelTree);
    connect(m_panelTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, Qt::UserRole).isValid()) return;
        if (m_panelKind == "buffers") switchBuffer(item->data(0, Qt::UserRole).toInt());
        else if (m_panelKind == "marks") jumpMark(item->data(0, Qt::UserRole).toString().at(0));
        else if (reader())
            reader()->jump(item->data(0, Qt::UserRole).toInt(), item->data(0, Qt::UserRole + 1).toPointF());
        if (reader()) reader()->setFocus();
    });
    splitter->addWidget(m_panel);
    m_panel->hide();

    m_stack = new QStackedWidget;
    m_welcome = new QWidget;
    m_welcome->setObjectName("welcome");
    auto *welcome = new QVBoxLayout(m_welcome);
    welcome->setContentsMargins(24, 24, 24, 24);
    welcome->setSpacing(14);
    welcome->addStretch();
    auto *brand = new QLabel("ivia");
    brand->setObjectName("brand");
    brand->setAlignment(Qt::AlignCenter);
    welcome->addWidget(brand);
    auto *openRow = new QHBoxLayout;
    openRow->addStretch();
    auto *open = button("Open a paper");
    open->setObjectName("welcomeOpen");
    connect(open, &QPushButton::clicked, this, &MainWindow::chooseFile);
    openRow->addWidget(open);
    openRow->addStretch();
    welcome->addLayout(openRow);
    auto *hint = new QLabel("drop a PDF  ·  :e to open  ·  :help");
    hint->setObjectName("welcomeHint");
    hint->setAlignment(Qt::AlignCenter);
    welcome->addWidget(hint);
    welcome->addStretch();
    m_stack->addWidget(m_welcome);
    splitter->addWidget(m_stack);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 920});
    layout->addWidget(splitter, 1);

    // These are children over the canvas, not rows in its layout. Entering a
    // command must not resize the PDF viewport or move the reading position.
    m_promptBar = new QFrame(central);
    m_promptBar->setObjectName("promptBar");
    m_promptBar->setFixedHeight(22);
    auto *promptRow = new QHBoxLayout(m_promptBar);
    promptRow->setContentsMargins(2, 0, 2, 0);
    promptRow->setSpacing(0);
    m_promptPrefix = new QLabel(":");
    m_promptPrefix->setObjectName("promptPrefix");
    m_prompt = new CommandLine;
    m_prompt->setObjectName("commandLine");
    m_prompt->setFrame(false);
    m_prompt->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_prompt->setClearButtonEnabled(false);
    promptRow->addWidget(m_promptPrefix);
    promptRow->addWidget(m_prompt, 1);
    connect(m_prompt, &QLineEdit::returnPressed, this, &MainWindow::submitPrompt);
    m_promptBar->hide();
    m_completionLabel = new QLabel(central);
    m_completionLabel->setObjectName("completionList");
    m_completionLabel->setTextFormat(Qt::PlainText);
    m_completionLabel->setWordWrap(true);
    m_completionLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_completionLabel->hide();
    connect(m_prompt, &CommandLine::cancelRequested, this, &MainWindow::leavePrompt);
    connect(m_prompt, &CommandLine::completionRequested, this, &MainWindow::completePrompt);
    connect(m_prompt, &CommandLine::completionListRequested, this, &MainWindow::listPromptCompletions);
    connect(m_prompt, &CommandLine::allCompletionsRequested, this, &MainWindow::insertAllCompletions);
    connect(m_prompt, &CommandLine::longestCompletionRequested, this, &MainWindow::longestCompletion);
    connect(m_prompt, &CommandLine::historyRequested, this, &MainWindow::recallPrompt);
    connect(m_prompt, &QLineEdit::textChanged, m_completionLabel, &QWidget::hide);

    m_messageLabel = new QLabel(central);
    m_messageLabel->setObjectName("notice");
    m_messageLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_messageLabel->hide();
    m_pendingLabel = new QLabel(central);
    m_pendingLabel->setObjectName("pendingKeys");
    m_pendingLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_pendingLabel->hide();

    // Native file shortcuts work even without an application toolbar or menu.
    auto *openAction = new QAction("Open PDF…", this);
#ifdef Q_OS_MACOS
    openAction->setShortcut(QKeySequence::Open);
#else
    openAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
#endif
    addAction(openAction);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseFile);
    auto *quitAction = new QAction("Quit Ivia", this);
    quitAction->setShortcut(QKeySequence::Quit);
    addAction(quitAction);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
#ifdef Q_OS_MACOS
    // The macOS menu is outside the document window. Offscreen captures and
    // other platforms should never gain an in-window menubar as a fallback.
    if (qApp->platformName() == "cocoa") {
        auto *fileMenu = menuBar()->addMenu("&File");
        fileMenu->addAction(openAction);
        fileMenu->addAction("Close buffer", this, &MainWindow::closeBuffer);
        fileMenu->addAction(quitAction);
    }
#endif
}

void MainWindow::applyTheme()
{
    const auto colors = theme(m_dark);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, colors.surface);
    palette.setColor(QPalette::WindowText, colors.ink);
    palette.setColor(QPalette::Base, colors.surface);
    palette.setColor(QPalette::Text, colors.ink);
    palette.setColor(QPalette::Button, colors.surface);
    palette.setColor(QPalette::ButtonText, colors.ink);
    palette.setColor(QPalette::Highlight, colors.accentWash);
    palette.setColor(QPalette::HighlightedText, colors.ink);
    setPalette(palette);
    setStyleSheet(QString(R"(
        QWidget { color: %1; font-family: 'Helvetica Neue', 'Segoe UI'; font-size: 13px; }
        QMainWindow, #canvas, #welcome { background: %6; }
        QPushButton { background: transparent; border: 0; padding: 7px 12px; border-radius: 4px; }
        QPushButton:hover { background: %7; }
        #brand { font-family: Georgia; font-size: 38px; color: %5; }
        #welcomeOpen { color: %1; }
        #welcomeHint { color: %2; font-size: 11px; }
        #promptBar { background: %6; border: 0; border-radius: 0; }
        #promptPrefix, #commandLine, #completionList { padding: 0; margin: 0; font-family: Menlo, Consolas, monospace; font-size: 13px; background: transparent; border: 0; }
        #promptPrefix { color: %1; }
        #completionList { color: %1; background: %6; border: 0; padding: 2px; }
        #commandLine { color: %1; selection-background-color: #dddddd; selection-color: #000000; }
        #notice { background: %8; color: %9; border: 0; border-radius: 0; padding: 7px 11px; font-size: 11px; }
        #pendingKeys { background: %8; color: %9; border-radius: 0; padding: 5px 9px; font-family: Menlo, Consolas, monospace; font-size: 11px; }
        #panel { background: %3; border-right: 1px solid %4; }
        #panelTitle { font-size: 10px; font-weight: 500; letter-spacing: 1px; color: %2; }
        QTreeWidget { border: 0; background: transparent; outline: 0; }
        QTreeWidget::item { padding: 8px 3px; }
        QTreeWidget::item:selected { color: %1; background: %7; border-radius: 4px; }
        QDialog { background: %3; }
        QLineEdit { background: %3; color: %1; selection-background-color: %7; selection-color: %1; }
        QTextBrowser { border: 0; background: transparent; padding: 18px; }
    )").arg(colors.ink.name(), colors.muted.name(), colors.surface.name(), colors.border.name(),
            colors.accent.name(), colors.canvas.name(), colors.accentWash.name(),
            colors.inputBackground.name(), colors.inputForeground.name()));
    for (auto &item : m_buffers) item->view->setDark(m_dark);
}

MainWindow::Buffer *MainWindow::buffer() const
{
    return m_active >= 0 && m_active < bufferCount() ? m_buffers[size_t(m_active)].get() : nullptr;
}
ReaderView *MainWindow::reader() const { return buffer() ? buffer()->view : nullptr; }
QString MainWindow::baseDirectory() const
{
    return buffer() ? QFileInfo(buffer()->path).absolutePath() : QDir::currentPath();
}

void MainWindow::chooseFile()
{
    chooseFiles(baseDirectory());
}

bool MainWindow::chooseFiles(const QString &directory)
{
    const auto paths = QFileDialog::getOpenFileNames(this, "Open papers", directory, "PDF documents (*.pdf)");
    bool opened = false;
    for (const auto &path : paths) opened = openFile(path) || opened;
    return opened;
}

bool MainWindow::openFile(const QString &input)
{
    const QFileInfo info(expandPath(input, baseDirectory()));
    if (info.isDir()) return chooseFiles(info.absoluteFilePath());
    if (!info.exists() || !info.isFile()) { showMessage("File not found: " + input); return false; }
    const QString path = info.canonicalFilePath();
    for (int i = 0; i < bufferCount(); ++i) {
        if (m_buffers[size_t(i)]->path == path) { switchBuffer(i); return true; }
    }
    auto document = std::make_unique<QPdfDocument>();
    auto error = document->load(path);
    while (error == QPdfDocument::Error::IncorrectPassword) {
        // A native-style password dialog keeps protected papers usable.
        QDialog dialog(this);
        dialog.setWindowTitle("Unlock PDF");
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel("Password for " + info.fileName()));
        auto *password = new QLineEdit;
        password->setEchoMode(QLineEdit::Password);
        layout->addWidget(password);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return false;
        document->close();
        document->setPassword(password->text());
        error = document->load(path);
    }
    if (error != QPdfDocument::Error::None || document->pageCount() == 0) {
        showMessage("Could not open PDF: " + info.fileName());
        return false;
    }
    auto item = std::make_unique<Buffer>();
    item->path = path;
    item->document = document.release();
    item->view = createView(item->document);
    auto *view = item->view;
    m_stack->addWidget(view);
    m_buffers.push_back(std::move(item));
    switchBuffer(bufferCount() - 1);
    const auto state = QJsonDocument::fromJson(QSettings().value(stateKey(path)).toByteArray()).object();
    const auto marks = state["marks"].toObject();
    for (auto it = marks.begin(); it != marks.end(); ++it)
        if (it.key().size() == 1) buffer()->marks[it.key().at(0)] = decodeLocation(it.value().toObject());
    if (state.contains("location")) {
        const auto location = decodeLocation(state["location"].toObject());
        view->restore(location);
    }
    if (!m_restoring) saveSession();
    return true;
}

ReaderView *MainWindow::createView(QPdfDocument *document)
{
    auto *view = new ReaderView(document);
    view->setDark(m_dark);
    connect(view, &ReaderView::positionChanged, this, &MainWindow::updateStatus);
    connect(view, &ReaderView::searchChanged, this, &MainWindow::updateStatus);
    connect(view, &ReaderView::message, this, &MainWindow::showMessage);
    connect(view, &ReaderView::hintsChanged, this, &MainWindow::updateStatus);
    connect(view, &ReaderView::previewRequested, this, &MainWindow::preview);
    connect(view, &ReaderView::externalLinkRequested, this, [this](const QUrl &url) {
        if (url.scheme() != "https" && url.scheme() != "http" && url.scheme() != "mailto") {
            showMessage("Unsupported link: " + url.toDisplayString());
            return;
        }
        QDesktopServices::openUrl(url);
    });
    return view;
}

void MainWindow::switchBuffer(int index)
{
    if (index < 0 || index >= bufferCount()) return;
    resetPending();
    if (reader()) reader()->cancelHints();
    m_active = index;
    m_stack->setCurrentWidget(reader());
    reader()->setFocus();
    setWindowTitle(QFileInfo(buffer()->path).fileName() + " — Ivia");
    if (m_panel->isVisible()) populatePanel();
    updateStatus();
}

void MainWindow::saveBuffer(Buffer *item)
{
    if (!item) return;
    QJsonObject marks;
    for (auto it = item->marks.cbegin(); it != item->marks.cend(); ++it)
        marks[QString(it.key())] = encodeLocation(it.value());
    QSettings().setValue(stateKey(item->path), QJsonDocument(QJsonObject{
        {"location", encodeLocation(item->view->location())}, {"marks", marks}}).toJson(QJsonDocument::Compact));
}

void MainWindow::saveSession()
{
    if (m_restoring) return;
    QStringList paths;
    for (const auto &item : m_buffers) { saveBuffer(item.get()); paths.append(item->path); }
    QSettings settings;
    settings.setValue("session/files", paths);
    settings.setValue("session/active", m_active);
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("appearance/dark", m_dark);
}

void MainWindow::restoreSession()
{
    const QSettings settings;
    const auto paths = settings.value("session/files").toStringList();
    const int active = settings.value("session/active", 0).toInt();
    m_restoring = true;
    for (const auto &path : paths) if (QFileInfo::exists(path)) openFile(path);
    m_restoring = false;
    switchBuffer(std::clamp(active, 0, std::max(0, bufferCount() - 1)));
}

void MainWindow::closeBuffer()
{
    if (!buffer()) return;
    const int index = m_active;
    saveBuffer(buffer());
    auto item = std::move(m_buffers[size_t(index)]);
    m_buffers.erase(m_buffers.begin() + index);
    m_active = -1;
    m_stack->removeWidget(item->view);
    delete item->view;
    delete item->document;
    if (bufferCount()) switchBuffer(std::min(index, bufferCount() - 1));
    else { m_stack->setCurrentWidget(m_welcome); m_panel->hide(); setWindowTitle("Ivia"); updateStatus(); }
    saveSession();
}

void MainWindow::positionOverlays()
{
    const QRect bounds = centralWidget()->rect();
    constexpr int inset = 16;
    m_promptBar->setGeometry(0, bounds.height() - m_promptBar->height(), bounds.width(), m_promptBar->height());
    m_completionLabel->setFixedWidth(bounds.width());
    const int completionHeight = std::min(bounds.height() / 3,
        std::max(26, m_completionLabel->heightForWidth(bounds.width())));
    m_completionLabel->setGeometry(0, m_promptBar->y() - completionHeight, bounds.width(), completionHeight);
    m_messageLabel->setMaximumWidth(bounds.width() - inset * 2);
    m_messageLabel->adjustSize();
    m_messageLabel->move(inset, bounds.height() - inset - m_messageLabel->height()
                         - (m_promptBar->isVisible() ? m_promptBar->height() + 8 : 0)
                         - (m_completionLabel->isVisible() ? m_completionLabel->height() : 0));
    m_pendingLabel->adjustSize();
    m_pendingLabel->move(bounds.width() - inset - m_pendingLabel->width(),
                         bounds.height() - inset - m_pendingLabel->height());
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    positionOverlays();
}

void MainWindow::updateStatus()
{
    const auto keys = m_count + m_pending;
    const bool pendingChanged = m_pendingLabel->text() != keys;
    m_pendingLabel->setText(keys);
    m_pendingLabel->setVisible(!keys.isEmpty() && m_promptKind.isNull());
    if (buffer()) {
        QString title = QString("%1 — %2/%3 — Ivia")
                .arg(QFileInfo(buffer()->path).fileName()).arg(reader()->currentPage() + 1)
                .arg(buffer()->document->pageCount());
        if (!reader()->searchText().isEmpty())
            title.prepend(QString("%1/%2 matches — ").arg(reader()->matchIndex() + 1).arg(reader()->matchCount()));
        if (windowTitle() != title) setWindowTitle(title);
    } else setWindowTitle("Ivia");
    if (pendingChanged) positionOverlays();
}

void MainWindow::showMessage(const QString &message)
{
    m_messageLabel->setText(QFontMetrics(m_messageLabel->font()).elidedText(
        message, Qt::ElideRight, std::max(160, centralWidget()->width() - 60)));
    m_messageLabel->setToolTip(message);
    m_messageLabel->show();
    m_messageLabel->raise();
    positionOverlays();
    m_messageTimer.start(3500);
}

void MainWindow::enterPrompt(QChar kind, QString initial)
{
    resetPending();
    if (reader()) reader()->cancelHints();
    m_promptKind = kind;
    m_promptPrefix->setText(QString(kind));
    m_prompt->setText(initial);
    m_prompt->setPlaceholderText({});
    m_completionText.clear();
    m_completions.clear();
    m_completionIndex = -1;
    m_messageTimer.stop();
    m_messageLabel->hide();
    m_historyIndex = m_history.size();
    m_historyDraft.clear();
    m_completionLabel->hide();
    m_promptBar->show();
    m_promptBar->raise();
    positionOverlays();
    m_prompt->setFocus();
    updateStatus();
}

void MainWindow::leavePrompt()
{
    m_promptKind = {};
    m_promptBar->hide();
    m_completionLabel->hide();
    m_messageTimer.stop();
    m_messageLabel->hide();
    if (reader()) reader()->setFocus();
    else centralWidget()->setFocus();
    updateStatus();
}

void MainWindow::submitPrompt()
{
    const auto text = m_prompt->text();
    const auto kind = m_promptKind;
    if (!text.trimmed().isEmpty()) {
        const auto entry = QString(kind) + text;
        if (m_history.isEmpty() || m_history.last() != entry) m_history.append(entry);
        if (m_history.size() > 200) m_history.removeFirst();
    }
    leavePrompt();
    if (kind == ':') executeCommand(text);
    else if (reader()) {
        const auto query = text.isEmpty() ? reader()->searchText() : text;
        reader()->search(query, kind == '?');
    }
}

QStringList MainWindow::promptCompletions() const
{
    if (m_promptKind != ':') return {};
    const auto text = m_prompt->text().left(m_prompt->cursorPosition());
    QStringList candidates;
    const auto separator = QRegularExpression("\\s+").match(text);
    if (!separator.hasMatch()) {
        for (const auto &name : commandNames()) if (name.startsWith(text)) candidates.append(name);
        return candidates;
    }
    const auto command = parseCommand(text);
    const auto prefix = text.left(separator.capturedEnd());
    if (command.name == "e" || command.name == "edit") {
        const auto rawArgument = text.mid(separator.capturedEnd());
        for (const auto &path : filePathCompletions(rawArgument, baseDirectory()))
            candidates.append(prefix + path);
    } else if (command.name == "b" || command.name == "buffer") {
        for (int i = 0; i < bufferCount(); ++i)
            if (QString::number(i + 1).startsWith(command.argument)
                    || QFileInfo(m_buffers[size_t(i)]->path).fileName().contains(command.argument, Qt::CaseInsensitive))
                candidates.append(prefix + QString::number(i + 1));
    }
    return candidates;
}

void MainWindow::replacePromptCompletion(const QString &prefix)
{
    // Complete only the part before the cursor; keep any suffix and preserve
    // the line editor's undo stack instead of replacing the entire input.
    m_prompt->setSelection(0, m_prompt->cursorPosition());
    m_prompt->insert(prefix);
    m_prompt->setCursorPosition(prefix.size());
    m_completionText = m_prompt->text();
    m_completionCursor = m_prompt->cursorPosition();
}

void MainWindow::completePrompt(bool reverse)
{
    if (m_promptKind != ':') return;
    if (!m_completions.isEmpty() && m_prompt->text() == m_completionText
            && m_prompt->cursorPosition() == m_completionCursor) {
        const int count = m_completions.size();
        m_completionIndex += reverse ? -1 : 1;
        if (m_completionIndex < -1) m_completionIndex = count - 1;
        if (m_completionIndex >= count) m_completionIndex = -1;
        replacePromptCompletion(m_completionIndex < 0 ? m_completionOriginal : m_completions.at(m_completionIndex));
        return;
    }
    const auto candidates = promptCompletions();
    if (candidates.isEmpty()) { showMessage("No completions"); return; }
    m_completions = candidates.size() > 1 ? candidates : QStringList{};
    m_completionOriginal = m_prompt->text().left(m_prompt->cursorPosition());
    m_completionIndex = -1;
    auto prefix = candidates.first();
    for (const auto &candidate : candidates)
        while (!candidate.startsWith(prefix) && !prefix.isEmpty()) prefix.chop(1);
    if (prefix.size() <= m_prompt->cursorPosition() || candidates.size() == 1) {
        m_completionIndex = reverse ? candidates.size() - 1 : 0;
        prefix = candidates.at(m_completionIndex);
    }
    replacePromptCompletion(prefix);
}

void MainWindow::listPromptCompletions()
{
    const auto candidates = promptCompletions();
    if (candidates.isEmpty()) { showMessage("No completions"); return; }
    QStringList display = candidates.mid(0, 50);
    if (candidates.size() > 50) display.append(QString("… %1 more").arg(candidates.size() - 50));
    m_messageTimer.stop();
    m_messageLabel->hide();
    m_completionLabel->setText(display.join("    "));
    m_completionLabel->show();
    m_completionLabel->raise();
    positionOverlays();
}

void MainWindow::insertAllCompletions()
{
    auto candidates = promptCompletions();
    if (candidates.isEmpty()) return;
    const auto before = m_prompt->text().left(m_prompt->cursorPosition());
    const auto separator = QRegularExpression("\\s+").match(before);
    QString prefix;
    if (separator.hasMatch()) {
        prefix = before.left(separator.capturedEnd());
        for (auto &candidate : candidates) candidate.remove(0, prefix.size());
    }
    replacePromptCompletion(prefix + candidates.join(' '));
    m_completions.clear();
}

void MainWindow::longestCompletion()
{
    const auto candidates = promptCompletions();
    if (candidates.isEmpty()) return;
    auto prefix = candidates.first();
    for (const auto &candidate : candidates)
        while (!candidate.startsWith(prefix) && !prefix.isEmpty()) prefix.chop(1);
    if (prefix.size() > m_prompt->cursorPosition()) replacePromptCompletion(prefix);
}

void MainWindow::recallPrompt(int direction, bool cycleCompletion)
{
    if (cycleCompletion && !m_completions.isEmpty() && m_prompt->text() == m_completionText
            && m_prompt->cursorPosition() == m_completionCursor) {
        completePrompt(direction < 0);
        return;
    }
    if (m_historyIndex == m_history.size()) m_historyDraft = m_prompt->text();
    int next = m_historyIndex + direction;
    while (next >= 0 && next < m_history.size() && !m_history.at(next).startsWith(m_promptKind)) next += direction;
    if (next >= 0 && next <= m_history.size()) {
        m_historyIndex = next;
        m_prompt->setText(next == m_history.size() ? m_historyDraft : m_history.at(next).mid(1));
        m_completions.clear();
        m_completionLabel->hide();
    }
}

bool MainWindow::executeCommand(const QString &text)
{
    const auto [name, argument] = parseCommand(text);
    if (name.isEmpty()) return true;
    if (name == "e" || name == "edit") {
        if (argument.isEmpty()) { chooseFile(); return true; }
        return openFile(argument);
    }
    if (name == "q" || name == "quit" || name == "qa") { close(); return true; }
    if (name == "help") { showHelp(); return true; }
    if (name == "buffers" || name == "ls" || ((name == "b" || name == "buffer") && argument.isEmpty())) { showPanel("buffers"); return true; }
    if (name == "set") {
        if (argument == "dark" || argument == "light") {
            m_dark = argument == "dark"; applyTheme(); saveSession(); return true;
        }
        showMessage("Options: :set light, :set dark"); return false;
    }
    if (!reader()) { showMessage("Open a PDF with :e path/to/paper.pdf"); return false; }
    if (name == "b" || name == "buffer") {
        bool numeric;
        int index = argument.toInt(&numeric) - 1;
        if (!numeric) {
            index = -1;
            for (int i = 0; i < bufferCount(); ++i)
                if (QFileInfo(m_buffers[size_t(i)]->path).fileName().contains(argument, Qt::CaseInsensitive)) {
                    if (index >= 0) { showMessage("Ambiguous buffer name. Use :b number"); return false; }
                    index = i;
                }
        }
        if (index < 0 || index >= bufferCount()) { showMessage("No such buffer"); return false; }
        switchBuffer(index); return true;
    }
    if (name == "bn" || name == "bnext") { switchBuffer((m_active + 1) % bufferCount()); return true; }
    if (name == "bp" || name == "bprevious") { switchBuffer((m_active + bufferCount() - 1) % bufferCount()); return true; }
    if (name == "bd" || name == "bdelete") { closeBuffer(); return true; }
    if (name == "toc" || name == "marks") { showPanel(name); return true; }
    if (name == "mark") {
        if (argument.size() != 1 || !argument.front().isLetter()) { showMessage("Usage: :mark a"); return false; }
        setMark(argument.at(0)); return true;
    }
    if (name == "delmark") { buffer()->marks.remove(argument.isEmpty() ? QChar{} : argument.at(0)); saveBuffer(buffer()); return true; }
    if (name == "nohlsearch" || name == "noh") { reader()->clearSearch(); return true; }
    if (name == "ref") { reader()->findReference(argument); return true; }
    if (name == "info") {
        showMessage(QString("%1  ·  %2/%3  ·  %4%")
            .arg(QFileInfo(buffer()->path).fileName()).arg(reader()->currentPage() + 1)
            .arg(buffer()->document->pageCount()).arg(qRound(reader()->zoom() * 100)));
        return true;
    }
    if (name == "fit") {
        if (argument.isEmpty() || argument == "width") reader()->fitWidth();
        else if (argument == "page") reader()->fitPage();
        else { showMessage("Usage: :fit width | page"); return false; }
        return true;
    }
    if (name == "zoom") {
        QString value = argument;
        value.remove('%');
        bool ok;
        const double percent = value.toDouble(&ok);
        if (!ok || !std::isfinite(percent) || percent < 10 || percent > 500) { showMessage("Zoom must be between 10 and 500 percent"); return false; }
        reader()->setZoom(percent / 100); return true;
    }
    if (name == "reload") {
        // Open a replacement completely before touching the active buffer. The
        // validated document itself is installed, avoiding a second-load race.
        auto replacement = std::make_unique<QPdfDocument>();
        replacement->setPassword(buffer()->document->password());
        if (replacement->load(buffer()->path) != QPdfDocument::Error::None || replacement->pageCount() == 0) {
            showMessage("Reload failed; current buffer kept"); return false;
        }
        const auto location = reader()->location();
        auto *view = createView(replacement.get());
        auto *previousView = buffer()->view;
        auto *previousDocument = buffer()->document;
        buffer()->view = view;
        buffer()->document = replacement.release();
        m_stack->addWidget(view);
        switchBuffer(m_active);
        m_stack->removeWidget(previousView);
        delete previousView;
        delete previousDocument;
        view->restore(location);
        showMessage("PDF reloaded");
        return true;
    }
    bool numeric;
    const int page = (name == "page" ? argument : name).toInt(&numeric);
    if (numeric) {
        if (page < 1 || page > buffer()->document->pageCount()) { showMessage("Page is outside this document"); return false; }
        reader()->jump(page - 1); return true;
    }
    showMessage("Unknown command: " + name + " · :help lists commands");
    return false;
}

void MainWindow::resetPending()
{
    m_pending.clear();
    m_count.clear();
    if (m_pendingLabel) { m_pendingLabel->clear(); m_pendingLabel->hide(); }
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) return QMainWindow::eventFilter(object, event);
    auto *widget = qobject_cast<QWidget *>(object);
    if (!widget || widget->window() != this || QApplication::activeModalWidget())
        return QMainWindow::eventFilter(object, event);
    auto *key = static_cast<QKeyEvent *>(event);
    if (m_promptBar->isVisible()) return false; // Never interpret propagated editing keys as reader commands.
    if (object == m_panelTree && key->key() == Qt::Key_Escape) {
        m_panel->hide(); if (reader()) reader()->setFocus(); return true;
    }
    if (object == m_panelTree && key->text() != ":" && key->text() != "/") {
        if (key->text() == "j" || key->text() == "k") {
            QKeyEvent arrow(QEvent::KeyPress, key->text() == "j" ? Qt::Key_Down : Qt::Key_Up, Qt::NoModifier);
            QApplication::sendEvent(m_panelTree, &arrow); return true;
        }
        return false;
    }
    if (qobject_cast<QLineEdit *>(object)) return false;
    normalKey(key);
    return key->isAccepted();
}

void MainWindow::normalKey(QKeyEvent *event)
{
    event->ignore();
    auto accept = [event] { event->accept(); };
    auto *view = reader();
    if (event->key() == Qt::Key_Escape) {
        resetPending(); if (view) { view->cancelHints(); view->clearSelection(); }
        m_panel->hide(); m_messageTimer.stop(); m_messageLabel->hide(); updateStatus(); accept(); return;
    }
    const auto modifiers = event->modifiers();
    if (modifiers & Qt::AltModifier) return;
    if (view && event->matches(QKeySequence::Copy)) {
        view->copySelection(); accept(); return;
    }
    if (CommandLine::physicalControl(modifiers)) {
        const int key = event->key();
        if (!view) return;
        int count = m_count.isEmpty() ? 1 : m_count.toInt();
        bool handled = true;
        if (key == Qt::Key_D) view->scrollBy(0, view->viewport()->height() / 2 * count);
        else if (key == Qt::Key_U) view->scrollBy(0, -view->viewport()->height() / 2 * count);
        else if (key == Qt::Key_F) view->scrollBy(0, view->viewport()->height() * count);
        else if (key == Qt::Key_B) view->scrollBy(0, -view->viewport()->height() * count);
        else if (key == Qt::Key_O) view->back();
        else if (key == Qt::Key_I) view->forward();
        else if (key == Qt::Key_C) view->copySelection();
        else if (key == Qt::Key_G) executeCommand("info");
        else handled = false;
        if (handled) { resetPending(); accept(); }
        return;
    }
    if (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) return;
    const QString text = event->text();
    if (view && view->hinting()) { if (!text.isEmpty()) view->hintKey(text); accept(); return; }
    if (text == ":" || text == "/" || text == "?") { enterPrompt(text.at(0)); accept(); return; }
    if (text.isEmpty()) return;
    if ((m_pending == "m" || m_pending == "'") && text.size() == 1) {
        if (view && text.at(0).isLetter()) {
            if (m_pending == "m") setMark(text.at(0)); else jumpMark(text.at(0));
        }
        resetPending(); accept(); return;
    }
    if (text.size() == 1 && text.front().isDigit() && (text != "0" || !m_count.isEmpty())) {
        if (m_count.size() < 4) m_count += text;
        updateStatus(); accept(); return;
    }
    const bool counted = !m_count.isEmpty();
    const int count = counted ? std::clamp(m_count.toInt(), 1, 9999) : 1;
    if (!m_pending.isEmpty()) {
        const auto sequence = m_pending + text;
        resetPending();
        if (view) {
            if (sequence == "gg") view->jump(counted ? count - 1 : 0);
            else if (sequence == "gd") view->followReference();
            else if (sequence == "gp") view->followReference(true);
            else if (sequence == "gt") switchBuffer((m_active + count) % bufferCount());
            else if (sequence == "gT") switchBuffer((m_active - count % bufferCount() + bufferCount()) % bufferCount());
            else if (sequence == "zi") view->setZoom(view->zoom() * 1.15);
            else if (sequence == "zo") view->setZoom(view->zoom() / 1.15);
            else if (sequence == "za") view->fitWidth();
            else if (sequence == "zz") view->fitPage();
            else showMessage("Unmapped key sequence: " + sequence);
        }
        accept(); return;
    }
    if (text == "g" || text == "z" || text == "m" || text == "'") {
        m_pending = text; updateStatus(); accept(); return;
    }
    if (!view) return;
    bool handled = true;
    if (text == "j") view->scrollBy(0, 48 * count);
    else if (text == "k") view->scrollBy(0, -48 * count);
    else if (text == "h") view->scrollBy(-48 * count, 0);
    else if (text == "l") view->scrollBy(48 * count, 0);
    else if (text == "G") view->jump(counted ? count - 1 : buffer()->document->pageCount() - 1);
    else if (text == "J") view->jump(view->currentPage() + count);
    else if (text == "K") view->jump(view->currentPage() - count);
    else if (text == " ") view->scrollBy(0, view->viewport()->height() * count);
    else if (text == "n") view->nextMatch(false, count);
    else if (text == "N") view->nextMatch(true, count);
    else if (text == "f") view->beginHints();
    else if (text == "F") view->beginHints(true);
    else if (text == "y") view->copySelection();
    else if (text == "+" || text == "=") view->setZoom(view->zoom() * std::pow(1.15, count));
    else if (text == "-") view->setZoom(view->zoom() / std::pow(1.15, count));
    else if (text == "0") view->fitWidth();
    else if (text == "t") showPanel("toc");
    else handled = false;
    resetPending();
    if (handled) accept();
}

void MainWindow::setMark(QChar mark)
{
    if (!buffer()) return;
    buffer()->marks[mark] = reader()->location();
    saveBuffer(buffer());
    showMessage("Mark " + QString(mark) + " set · '" + QString(mark) + " returns here");
    if (m_panel->isVisible() && m_panelKind == "marks") populatePanel();
}

void MainWindow::jumpMark(QChar mark)
{
    if (!buffer()) return;
    if (!buffer()->marks.contains(mark)) { showMessage("Mark is not set: " + QString(mark)); return; }
    const auto location = buffer()->marks.value(mark);
    reader()->jump(location.page, location.point);
    reader()->restore(location);
}

void MainWindow::showPanel(const QString &kind)
{
    if (m_panel->isVisible() && m_panelKind == kind) { m_panel->hide(); if (reader()) reader()->setFocus(); return; }
    m_panelKind = kind;
    populatePanel();
    m_panel->show();
    m_panelTree->setFocus();
    if (m_panelTree->topLevelItemCount()) m_panelTree->setCurrentItem(m_panelTree->topLevelItem(0));
}

void MainWindow::populatePanel()
{
    m_panelTitle->setText(m_panelKind == "toc" ? "CONTENTS" : m_panelKind.toUpper());
    m_panelTree->clear();
    if (m_panelKind == "buffers") {
        for (int i = 0; i < bufferCount(); ++i) {
            auto *row = new QTreeWidgetItem(m_panelTree, {QString("%1   %2").arg(i + 1).arg(QFileInfo(m_buffers[size_t(i)]->path).completeBaseName())});
            row->setData(0, Qt::UserRole, i);
            row->setToolTip(0, m_buffers[size_t(i)]->path);
        }
    } else if (buffer() && m_panelKind == "marks") {
        for (auto it = buffer()->marks.cbegin(); it != buffer()->marks.cend(); ++it) {
            auto *row = new QTreeWidgetItem(m_panelTree, {QString("%1   Page %2").arg(it.key()).arg(it.value().page + 1)});
            row->setData(0, Qt::UserRole, QString(it.key()));
        }
    } else if (buffer()) {
        QPdfBookmarkModel model;
        model.setDocument(buffer()->document);
        std::function<void(QModelIndex, QTreeWidgetItem *)> append = [&](QModelIndex parent, QTreeWidgetItem *node) {
            for (int row = 0; row < model.rowCount(parent); ++row) {
                const auto index = model.index(row, 0, parent);
                const auto title = model.data(index, int(QPdfBookmarkModel::Role::Title)).toString();
                auto *item = node ? new QTreeWidgetItem(node, {title}) : new QTreeWidgetItem(m_panelTree, {title});
                item->setData(0, Qt::UserRole, model.data(index, int(QPdfBookmarkModel::Role::Page)));
                item->setData(0, Qt::UserRole + 1, model.data(index, int(QPdfBookmarkModel::Role::Location)));
                append(index, item);
            }
        };
        append({}, nullptr);
        m_panelTree->expandToDepth(0);
    }
    if (!m_panelTree->topLevelItemCount()) {
        auto *empty = new QTreeWidgetItem(m_panelTree, {m_panelKind == "toc" ? "No embedded contents" : "Nothing here yet"});
        empty->setFlags(Qt::NoItemFlags);
    }
}

void MainWindow::preview(int page, QPointF point)
{
    if (!buffer()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QString("Reference preview · page %1").arg(page + 1));
    dialog.resize(680, 700);
    auto *layout = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(false);
    auto *image = new QLabel;
    const auto size = buffer()->document->pagePointSize(page);
    const qreal scale = std::min(1.5, 4000.0 / std::max(size.width(), size.height()));
    const auto rendered = buffer()->document->render(page, (size * scale).toSize());
    image->setPixmap(QPixmap::fromImage(rendered));
    scroll->setWidget(image);
    layout->addWidget(scroll);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *go = buttons->addButton("Jump to reference", QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(go, &QPushButton::clicked, &dialog, &QDialog::accept);
    QTimer::singleShot(0, &dialog, [scroll, point, scale] { scroll->verticalScrollBar()->setValue(int(point.y() * scale) - 50); });
    if (dialog.exec() == QDialog::Accepted && reader()) reader()->jump(page, point);
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Ivia · field guide");
    dialog.resize(660, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QTextBrowser;
    text->setHtml(R"(
    <style>body{font-size:14px;}h1{font-family:Georgia;font-weight:normal;font-size:32px;}
    h2{font-size:15px;margin-top:24px;}td{padding:6px 18px 6px 0;}</style>
    <h1>A little fluency goes a long way.</h1>
    <p>Ivia is a keyboard-first home for academic PDFs. Press <code>Esc</code> to return to normal mode.</p>
    <h2>Move through a paper</h2><table>
    <tr><td><code>h j k l</code></td><td>Scroll left, down, up, right. Counts work: <code>8j</code>.</td></tr>
    <tr><td><code>Ctrl+D / Ctrl+U</code></td><td>Half page down / up</td></tr>
    <tr><td><code>Ctrl+F / Ctrl+B</code></td><td>Full page down / up. Space also moves down.</td></tr>
    <tr><td><code>gg / G / 12G</code></td><td>First page / last page / page 12</td></tr>
    <tr><td><code>J / K</code></td><td>Next / previous page</td></tr>
    <tr><td><code>+ / − / 0</code></td><td>Zoom in / out / fit width</td></tr>
    <tr><td><code>zi / zo / za / zz</code></td><td>Zoom in / out / fit width / fit page</td></tr></table>
    <h2>Find, follow, return</h2><table>
    <tr><td><code>/text / ?text</code></td><td>Search forward / backward; Enter commits</td></tr>
    <tr><td><code>n / N</code></td><td>Next / previous match, wrapping through the paper</td></tr>
    <tr><td><code>f / F</code></td><td>Type a visible link's label to follow / preview</td></tr>
    <tr><td><code>gd / gp</code></td><td>Follow / preview the link under the pointer; otherwise show hints</td></tr>
    <tr><td><code>gd / :ref 12</code></td><td>Look up a selected numbered citation / reference 12</td></tr>
    <tr><td><code>Ctrl+O / Ctrl+I</code></td><td>Back / forward through jumps</td></tr>
    <tr><td><code>ma / 'a</code></td><td>Set / return to mark a (saved for each paper)</td></tr>
    <tr><td><code>y</code></td><td>Copy mouse-selected text</td></tr></table>
    <h2>The command line</h2><table>
    <tr><td><code>:e path/to/file.pdf</code></td><td>Open a PDF; spaces, quotes and ~/ work</td></tr>
    <tr><td><code>:b / :b 2 / :b name</code></td><td>List / switch buffers. Also <code>gt</code> and <code>gT</code>.</td></tr>
    <tr><td><code>:bn / :bp / :bd</code></td><td>Next / previous / close buffer</td></tr>
    <tr><td><code>:42 / :page 42</code></td><td>Jump to physical page 42</td></tr>
    <tr><td><code>:toc / :marks</code></td><td>Contents / saved marks. j/k and Enter navigate panels.</td></tr>
    <tr><td><code>:zoom 125 / :fit page</code></td><td>Zoom percent / fit page. <code>:fit width</code> also works.</td></tr>
    <tr><td><code>:noh / :reload</code></td><td>Clear search highlights / reload PDF from disk</td></tr>
    <tr><td><code>:set light / :set dark</code></td><td>Set the interface theme; paper retains its original colors</td></tr>
    <tr><td><code>Ctrl+G / :info</code></td><td>Briefly show filename, page and zoom</td></tr>
    <tr><td><code>:q / :qa</code></td><td>Quit, saving the reading session</td></tr></table>
    <h2>Editing the command line</h2><table>
    <tr><td><code>Ctrl+H / W / U</code></td><td>Backspace / delete previous word / delete before cursor</td></tr>
    <tr><td><code>Ctrl+B / E</code></td><td>Beginning / end of line</td></tr>
    <tr><td><code>Ctrl+D</code></td><td>List matching commands or paths without changing the input</td></tr>
    <tr><td><code>Ctrl+A / L</code></td><td>Insert all matches / longest common completion</td></tr>
    <tr><td><code>Ctrl+N / P</code></td><td>Cycle active completions, otherwise prompt history</td></tr>
    <tr><td><code>Ctrl+J / M</code></td><td>Submit the command</td></tr>
    <tr><td><code>Ctrl+C / [</code></td><td>Cancel the command</td></tr></table>
    <p>Paths support ~/, Unix ~username, $VAR, ${VAR}, and quoted names. Use :e ~ to browse home.</p>
    <p>Tab / Shift+Tab cycle command and file completions. ↑/↓ recall prompt history. Reading positions, zoom,
    buffers and marks are restored next launch. PDFs are opened read-only.</p>
    <p>On macOS, Ctrl means the physical Control key. Command+O opens files.</p>
    <p>Citation fallback recognizes numbered references in text PDFs. Scans need an existing OCR text layer.
    Author–year citation inference and PDF annotation editing are not part of this version.</p>
    )");
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSession();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        for (const auto &url : event->mimeData()->urls())
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
                event->acceptProposedAction(); return;
            }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).suffix().compare("pdf", Qt::CaseInsensitive) == 0)
            openFile(url.toLocalFile());
    event->acceptProposedAction();
}
}
