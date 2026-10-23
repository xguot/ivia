#include "main_window.h"
#include "demo.h"
#include "theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileOpenEvent>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>

class IviaApplication final : public QApplication {
public:
    using QApplication::QApplication;
    ivia::MainWindow *window = nullptr;
protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::FileOpen && window) {
            window->openFile(static_cast<QFileOpenEvent *>(event)->file());
            window->show();
            window->raise();
            window->activateWindow();
            return true;
        }
        return QApplication::event(event);
    }
};

int main(int argc, char **argv)
{
    IviaApplication app(argc, argv);
    QApplication::setApplicationName("Ivia");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("ivia");
    QApplication::setOrganizationDomain("ivia.local");
    QApplication::setStyle("Fusion");
    QPixmap icon(128, 128);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#ded5ec"));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(4, 4, 120, 120), 27, 27);
    painter.setPen(ivia::theme(false).accent);
    painter.setFont(QFont("Georgia", 76));
    painter.drawText(icon.rect().adjusted(0, -9, 0, 0), Qt::AlignCenter, "i");
    painter.end();
    app.setWindowIcon(QIcon(icon));
    QCommandLineParser parser;
    parser.setApplicationDescription("A quiet, Vim-inspired academic PDF reader.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"demo", "Open the built-in reading guide."});
    parser.addOption({"no-session", "Start without restoring the previous reading session."});
    parser.addPositionalArgument("files", "PDF documents to open.", "[files…]");
    parser.process(app);

    ivia::MainWindow window;
    app.window = &window;
    window.show();
    if (!parser.positionalArguments().isEmpty()) {
        for (const auto &file : parser.positionalArguments()) window.openFile(QFileInfo(file).absoluteFilePath());
    } else if (parser.isSet("demo")) {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(directory);
        const auto path = directory + "/Ivia field notes.pdf";
        if (ivia::createDemo(path)) window.openFile(path);
        else window.showMessage("Could not create the reading guide");
    } else if (!parser.isSet("no-session")) window.restoreSession();
    return app.exec();
}
