#include "command.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <pwd.h>
#include <unistd.h>
#include <vector>
#endif

namespace ivia {
namespace {
struct PathArgument {
    QString path;
    QChar quote;
    bool closed = false;
};

PathArgument pathArgument(QString argument)
{
    PathArgument result{argument, {}, false};
    if (argument.startsWith('"') || argument.startsWith('\'')) {
        result.quote = argument.front();
        argument.remove(0, 1);
        if (argument.endsWith(result.quote)) {
            result.closed = true;
            argument.chop(1);
        }
        result.path = argument;
    }
    return result;
}

QString expandEnvironment(const QString &path)
{
    static const QRegularExpression variable(R"(\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*)))");
    QString result;
    qsizetype previous = 0;
    auto matches = variable.globalMatch(path);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += path.mid(previous, match.capturedStart() - previous);
        const auto name = (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toLatin1();
        // Unknown variables stay visible instead of silently resolving to a
        // different file. Expansion is a single pass and never invokes a shell.
        result += qEnvironmentVariableIsSet(name.constData())
            ? qEnvironmentVariable(name.constData()) : match.captured();
        previous = match.capturedEnd();
    }
    result += path.mid(previous);
    return result;
}

QString userHome(const QString &user)
{
    if (user.isEmpty()) return QDir::homePath();
#ifdef Q_OS_UNIX
    const auto name = user.toLocal8Bit();
    const auto suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t size = suggested > 0 ? size_t(suggested) : 16384;
    // Use the reentrant API: completion may run alongside Qt worker threads.
    while (size <= 1024 * 1024) {
        std::vector<char> storage(size);
        passwd entry{};
        passwd *result = nullptr;
        const int error = getpwnam_r(name.constData(), &entry, storage.data(), storage.size(), &result);
        if (!error) return result ? QString::fromLocal8Bit(result->pw_dir) : QString{};
        if (error != ERANGE) break;
        size *= 2;
    }
#endif
    return {};
}

QString expandHome(QString path)
{
    if (!path.startsWith('~')) return path;
    const auto slash = path.indexOf('/');
    const auto name = path.mid(1, slash < 0 ? -1 : slash - 1);
    const auto home = userHome(name);
    if (!home.isEmpty()) path = home + (slash < 0 ? QString{} : path.mid(slash));
    return path;
}

QString completedArgument(const PathArgument &argument, const QString &path, bool directory)
{
    if (argument.quote.isNull()) return path;
    return QString(argument.quote) + path
        + (argument.closed || !directory ? QString(argument.quote) : QString{});
}
}

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
    path = expandHome(expandEnvironment(pathArgument(path).path));
    return QDir::cleanPath(QDir(baseDirectory.isEmpty() ? QDir::currentPath() : baseDirectory)
                              .absoluteFilePath(path));
}

QStringList filePathCompletions(const QString &text, const QString &baseDirectory, bool directoriesOnly)
{
    const auto argument = pathArgument(text);
    const auto &path = argument.path;
    const auto expanded = expandPath(path, baseDirectory);
    const QFileInfo info(expanded);
    const bool trailingSlash = path.endsWith('/');
    static const QRegularExpression environmentRoot(R"(^\$(?:[A-Za-z_][A-Za-z0-9_]*|\{[A-Za-z_][A-Za-z0-9_]*\})$)");
    // Roots are complete directory names, not filename prefixes. The next Tab
    // can then enumerate their contents without expanding the displayed path.
    const bool homeRoot = path.startsWith('~') && !path.contains('/') && expandHome(path) != path;
    if (!trailingSlash && info.isDir()
            && (homeRoot || environmentRoot.match(path).hasMatch() || path == "." || path == ".."))
        return {completedArgument(argument, path + '/', true)};

    const QDir directory(trailingSlash || path.isEmpty() ? expanded : info.absolutePath());
    const auto partial = trailingSlash || path.isEmpty() ? QString{} : info.fileName();
    const auto prefix = trailingSlash ? path : path.left(path.lastIndexOf('/') + 1);
    const bool keepPrefix = expandPath(prefix, baseDirectory) == directory.absolutePath();
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (!directoriesOnly) filters |= QDir::Files;
    if (partial.startsWith('.')) filters |= QDir::Hidden;
    QStringList candidates;
    for (const auto &entry : directory.entryInfoList(filters, QDir::Name | QDir::IgnoreCase)) {
        if (!entry.fileName().startsWith(partial, Qt::CaseInsensitive)
                || (!entry.isDir() && entry.suffix().compare("pdf", Qt::CaseInsensitive) != 0))
            continue;
        QString candidate;
        if (keepPrefix) {
            candidate = prefix + entry.fileName();
        } else if (!trailingSlash && entry.absoluteFilePath().startsWith(expanded)) {
            // A variable may itself contain several path components. Retain
            // that notation when the completion can be expressed as a suffix.
            candidate = path;
            const auto suffix = entry.absoluteFilePath().mid(expanded.size());
            static const QRegularExpression bareVariable(R"(\$([A-Za-z_][A-Za-z0-9_]*)$)");
            const auto match = bareVariable.match(candidate);
            if (match.hasMatch() && !suffix.isEmpty()
                    && (suffix.front().isLetterOrNumber() || suffix.front() == '_'))
                candidate.replace(match.capturedStart(), match.capturedLength(), "${" + match.captured(1) + '}');
            candidate += suffix;
        } else {
            candidate = entry.absoluteFilePath();
        }
        if (entry.isDir()) candidate += '/';
        candidates.append(completedArgument(argument, candidate, entry.isDir()));
    }
    return candidates;
}

QStringList commandNames()
{
    return {"e", "edit", "cd", "chdir", "pwd", "b", "buffer", "buffers", "bn", "bnext", "bp", "bprevious",
            "bd", "bdelete", "q", "quit", "qa", "page", "zoom", "fit", "files", "ls", "toc", "marks",
            "mark", "delmark", "nohlsearch", "noh", "set", "help", "reload", "ref", "info"};
}
}
