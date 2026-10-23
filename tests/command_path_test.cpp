#include "command.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_UNIX
#include <pwd.h>
#include <unistd.h>
#endif

using namespace ivia;

class CommandPathTest : public QObject {
    Q_OBJECT
private slots:
    void pathExpansion()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto name = "IVIA_TEST_PATH_ROOT";
        const auto previous = qgetenv(name);
        const bool wasSet = qEnvironmentVariableIsSet(name);
        qputenv(name, temporary.path().toUtf8());
        QCOMPARE(expandPath("$IVIA_TEST_PATH_ROOT/a paper.pdf"), temporary.path() + "/a paper.pdf");
        QCOMPARE(expandPath("${IVIA_TEST_PATH_ROOT}/paper.pdf"), temporary.path() + "/paper.pdf");
        if (wasSet) qputenv(name, previous); else qunsetenv(name);
        QCOMPARE(expandPath("~"), QDir::homePath());
        QCOMPARE(expandPath("\"~/a paper.pdf\""), QDir::homePath() + "/a paper.pdf");
        QCOMPARE(expandPath("'~/unfinished path"), QDir::homePath() + "/unfinished path");
        QCOMPARE(expandPath("../paper.pdf", temporary.path() + "/papers"), temporary.path() + "/paper.pdf");
        QCOMPARE(expandPath("$IVIA_UNSET_VARIABLE_42891/file.pdf", temporary.path()),
                 temporary.path() + "/$IVIA_UNSET_VARIABLE_42891/file.pdf");
        QCOMPARE(expandPath("~ivia_user_does_not_exist_42891/file.pdf", temporary.path()),
                 temporary.path() + "/~ivia_user_does_not_exist_42891/file.pdf");
    }

    void completions()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QDir dir(temporary.path());
        QVERIFY(dir.mkdir("papers"));
        QVERIFY(dir.mkdir("paper archive"));
        for (const auto &name : {"paper one.pdf", "paper two.PDF", "other.txt", ".hidden.pdf"}) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const QStringList expected{"paper archive/", "paper one.pdf", "paper two.PDF", "papers/"};
        QCOMPARE(filePathCompletions("pa", temporary.path()), expected);
        QCOMPARE(filePathCompletions({}, temporary.path()), expected);
        QCOMPARE(filePathCompletions("./pa", temporary.path()),
                 (QStringList{"./paper archive/", "./paper one.pdf", "./paper two.PDF", "./papers/"}));
        QCOMPARE(filePathCompletions("'pa", temporary.path()),
                 (QStringList{"'paper archive/", "'paper one.pdf'", "'paper two.PDF'", "'papers/"}));
        QCOMPARE(filePathCompletions("\"pa\"", temporary.path()),
                 (QStringList{"\"paper archive/\"", "\"paper one.pdf\"", "\"paper two.PDF\"", "\"papers/\""}));
        QCOMPARE(filePathCompletions(".", temporary.path()), QStringList{"./"});
        QCOMPARE(filePathCompletions(".h", temporary.path()), QStringList{".hidden.pdf"});
        QCOMPARE(filePathCompletions("~", temporary.path()), QStringList{"~/"});
        QCOMPARE(filePathCompletions("'~", temporary.path()), QStringList{"'~/"});
        QVERIFY(filePathCompletions("missing/", temporary.path()).isEmpty());
        QVERIFY(filePathCompletions("papers/", temporary.path()).isEmpty());
    }

    void environmentCompletions()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile file(temporary.filePath("paper.pdf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto name = "IVIA_TEST_PATH_ROOT";
        const auto previous = qgetenv(name);
        const bool wasSet = qEnvironmentVariableIsSet(name);
        qputenv(name, temporary.path().toUtf8());
        const auto root = filePathCompletions("$IVIA_TEST_PATH_ROOT");
        const auto prefix = filePathCompletions("${IVIA_TEST_PATH_ROOT}/pa");
        qputenv(name, (temporary.path() + "/pa").toUtf8());
        const auto partial = filePathCompletions("$IVIA_TEST_PATH_ROOT");
        if (wasSet) qputenv(name, previous); else qunsetenv(name);
        QCOMPARE(root, QStringList{"$IVIA_TEST_PATH_ROOT/"});
        QCOMPARE(prefix, QStringList{"${IVIA_TEST_PATH_ROOT}/paper.pdf"});
        QCOMPARE(partial, QStringList{"${IVIA_TEST_PATH_ROOT}per.pdf"});
    }

    void directoryCompletions()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QDir directory(temporary.path());
        QVERIFY(directory.mkdir("paper archive"));
        QVERIFY(directory.mkdir("papers"));
        QVERIFY(directory.mkdir(".hidden"));
        for (const auto &name : {"paper.pdf", "paper.PDF", "paper.txt"}) {
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        const QStringList expected{"paper archive/", "papers/"};
        QCOMPARE(filePathCompletions({}, temporary.path(), true), expected);
        QCOMPARE(filePathCompletions("pa", temporary.path(), true), expected);
        QCOMPARE(filePathCompletions("./pa", temporary.path(), true),
                 (QStringList{"./paper archive/", "./papers/"}));
        QCOMPARE(filePathCompletions("'pa", temporary.path(), true),
                 (QStringList{"'paper archive/", "'papers/"}));
        QCOMPARE(filePathCompletions("\"pa\"", temporary.path(), true),
                 (QStringList{"\"paper archive/\"", "\"papers/\""}));
        QCOMPARE(filePathCompletions(".h", temporary.path(), true), QStringList{".hidden/"});
        QCOMPARE(filePathCompletions("~", temporary.path(), true), QStringList{"~/"});
        QCOMPARE(filePathCompletions("'~", temporary.path(), true), QStringList{"'~/"});
        QVERIFY(filePathCompletions("paper.p", temporary.path(), true).isEmpty());

        const auto name = "IVIA_TEST_DIRECTORY_ROOT";
        const auto previous = qgetenv(name);
        const bool wasSet = qEnvironmentVariableIsSet(name);
        qputenv(name, temporary.path().toUtf8());
        const auto root = filePathCompletions("$IVIA_TEST_DIRECTORY_ROOT", {}, true);
        const auto prefix = filePathCompletions("${IVIA_TEST_DIRECTORY_ROOT}/pa", {}, true);
        const auto quoted = filePathCompletions("\"$IVIA_TEST_DIRECTORY_ROOT/pa", {}, true);
        qputenv(name, (temporary.path() + "/pa").toUtf8());
        const auto partial = filePathCompletions("$IVIA_TEST_DIRECTORY_ROOT", {}, true);
        if (wasSet) qputenv(name, previous); else qunsetenv(name);
        QCOMPARE(root, QStringList{"$IVIA_TEST_DIRECTORY_ROOT/"});
        QCOMPARE(prefix, (QStringList{"${IVIA_TEST_DIRECTORY_ROOT}/paper archive/",
                                      "${IVIA_TEST_DIRECTORY_ROOT}/papers/"}));
        QCOMPARE(quoted, (QStringList{"\"$IVIA_TEST_DIRECTORY_ROOT/paper archive/",
                                      "\"$IVIA_TEST_DIRECTORY_ROOT/papers/"}));
        QCOMPARE(partial, (QStringList{"${IVIA_TEST_DIRECTORY_ROOT}per archive/",
                                       "${IVIA_TEST_DIRECTORY_ROOT}pers/"}));
    }

    void namedHome()
    {
#ifdef Q_OS_UNIX
        const auto *user = getpwuid(getuid());
        QVERIFY(user);
        const auto name = QString::fromLocal8Bit(user->pw_name);
        const auto home = QString::fromLocal8Bit(user->pw_dir);
        QCOMPARE(expandPath('~' + name + "/paper.pdf"), home + "/paper.pdf");
        QCOMPARE(filePathCompletions('~' + name), QStringList{'~' + name + '/'});
#else
        QSKIP("Named home directories are available on Unix.");
#endif
    }
};

QTEST_APPLESS_MAIN(CommandPathTest)
#include "command_path_test.moc"
