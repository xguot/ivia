#pragma once
#include <QString>
#include <QStringList>

namespace ivia {
struct Command {
    QString name;
    QString argument;
};
Command parseCommand(QString text);
// Resolve relative paths, home notation and $VAR/${VAR}; accept quoted paths
// without executing a shell. Unknown users and variables remain literal.
QString expandPath(QString path, const QString &baseDirectory = {});
// PDF files and directories matching a command-line path. Results contain only
// the argument, retain relative/home/environment prefixes and quote style, and
// end directories with '/'. An unfinished quote is closed for completed files.
QStringList filePathCompletions(const QString &argument, const QString &baseDirectory = {});
QStringList commandNames();
}
