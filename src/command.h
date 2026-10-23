#pragma once
#include <QString>
#include <QStringList>

namespace ivia {
struct Command {
    QString name;
    QString argument;
};
Command parseCommand(QString text);
QString expandPath(QString path, const QString &baseDirectory = {});
QStringList commandNames();
}
