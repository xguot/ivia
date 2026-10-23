#include "command.h"
#include <QDir>
#include <QRegularExpression>

namespace ivia {
Command parseCommand(QString text)
{
    text = text.trimmed();
    if (text.startsWith(':')) text.remove(0, 1);
    const auto separator = text.indexOf(QRegularExpression("\\s"));
    if (separator < 0) return {text, {}};
    QString argument = text.mid(separator + 1).trimmed();
    if (argument.size() >= 2 && ((argument.front() == '"' && argument.back() == '"')
            || (argument.front() == '\'' && argument.back() == '\'')))
        argument = argument.mid(1, argument.size() - 2);
    return {text.left(separator), argument};
}

QString expandPath(QString path, const QString &baseDirectory)
{
    if (path == "~") path = QDir::homePath();
    else if (path.startsWith("~/")) path = QDir::homePath() + path.mid(1);
    return QDir::cleanPath(QDir(baseDirectory.isEmpty() ? QDir::currentPath() : baseDirectory)
                              .absoluteFilePath(path));
}

QStringList commandNames()
{
    return {"e", "edit", "b", "buffer", "buffers", "bn", "bnext", "bp", "bprevious",
            "bd", "bdelete", "q", "quit", "qa", "page", "zoom", "fit", "toc", "marks",
            "mark", "delmark", "nohlsearch", "noh", "set", "help", "reload", "ref", "info"};
}
}
