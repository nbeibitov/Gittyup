//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "git/Command.h"
#include "git/Commit.h"
#include "git/Filter.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "git2/errors.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

// Registers the long-running filters of filterServer in a fake $HOME.
// git::Filter::init() reads the global filters once, when the application
// is created, so $HOME is redirected during static initialization, like in
// the Filter test.
struct FakeHome {
  QTemporaryDir dir;
  QString log;

  FakeHome() {
    log = dir.filePath("filter.log");
    qputenv("GITTYUP_FILTER_LOG", QDir::toNativeSeparators(log).toLocal8Bit());

    QString server = QDir::fromNativeSeparators(FILTER_SERVER);
    QFile gitconfig(dir.filePath(".gitconfig"));
    bool opened = gitconfig.open(QIODevice::WriteOnly | QIODevice::Truncate);
    Q_ASSERT(opened);
    Q_UNUSED(opened);

    QTextStream out(&gitconfig);
    out << "[filter \"gittyuprequired\"]\n";
    out << "    process = '" << server << "'\n";
    out << "    required = true\n";
    out << "[filter \"gittyupoptional\"]\n";
    out << "    process = '" << server << "'\n";
    gitconfig.close();

    qputenv("HOME", dir.path().toLocal8Bit());
    qputenv("USERPROFILE", dir.path().toLocal8Bit());
  }

  QStringList lines() const {
    QFile file(log);
    if (!file.open(QFile::ReadOnly))
      return QStringList();
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
  }
};

FakeHome &fakeHome() {
  static FakeHome home;
  return home;
}

// Force construction (and therefore the $HOME override) before main().
FakeHome &gFakeHomeInit = fakeHome();

} // namespace

class TestFilterProcess : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void oneProcessForAllFiles();
  void optionalFilterPassesThrough();
  void requiredFilterFails();

private:
  void writeFile(const QString &name, const QByteArray &content);
  QByteArray readFile(const QString &name) const;

  QScopedPointer<Test::ScratchRepository> mScratch;
  git::Repository mRepo;
};

void TestFilterProcess::init() {
  if (git::Command::bashPath().isEmpty())
    QSKIP("bash is not installed");

  QFile::remove(fakeHome().log);
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;

  writeFile(".gitattributes", "*.req filter=gittyuprequired\n"
                              "*.opt filter=gittyupoptional\n");
  mRepo.index().setStaged({".gitattributes"}, true);
  QVERIFY(mRepo.commit("attributes").isValid());
}

void TestFilterProcess::cleanup() {
  git::Filter::stopProcesses();
  mRepo = git::Repository();
  mScratch.reset();
}

void TestFilterProcess::writeFile(const QString &name,
                                  const QByteArray &content) {
  QFile file(mRepo.workdir().filePath(name));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  file.write(content);
}

QByteArray TestFilterProcess::readFile(const QString &name) const {
  QFile file(mRepo.workdir().filePath(name));
  return file.open(QFile::ReadOnly) ? file.readAll() : QByteArray();
}

void TestFilterProcess::oneProcessForAllFiles() {
  // Cleaning removes what smudging adds.
  QStringList names = {"a.req", "b.req", "c.req"};
  for (const QString &name : names)
    writeFile(name, "S:" + name.toUtf8());
  mRepo.index().setStaged(names, true);
  for (const QString &name : names) {
    const git_error *error = git_error_last();
    QVERIFY2(mRepo.index().isTracked(name),
             error && error->message ? error->message : "");
  }
  git::Commit commit = mRepo.commit("files");
  QVERIFY(commit.isValid());
  for (const QString &name : names)
    QCOMPARE(commit.blob(name).content(), name.toUtf8());

  // Checking out smudges them again.
  for (const QString &name : names)
    QVERIFY(QFile::remove(mRepo.workdir().filePath(name)));
  QVERIFY(mRepo.checkout(commit, nullptr, QStringList(), GIT_CHECKOUT_FORCE));
  for (const QString &name : names)
    QCOMPARE(readFile(name), "S:" + name.toUtf8());

  // One process filtered all of them.
  QStringList lines = fakeHome().lines();
  QCOMPARE(lines.count("start"), 1);
  for (const QString &name : names) {
    QVERIFY2(lines.contains("clean " + name), qPrintable(lines.join(", ")));
    QVERIFY2(lines.contains("smudge " + name), qPrintable(lines.join(", ")));
  }
}

// Like git, an optional filter that fails leaves the content as it is.
void TestFilterProcess::optionalFilterPassesThrough() {
  writeFile("fail.opt", "content");
  mRepo.index().setStaged({"fail.opt"}, true);
  git::Commit commit = mRepo.commit("optional");
  QVERIFY(commit.isValid());
  QCOMPARE(commit.blob("fail.opt").content(), QByteArray("content"));
}

void TestFilterProcess::requiredFilterFails() {
  writeFile("fail.req", "content");
  mRepo.index().setStaged({"fail.req"}, true);
  QVERIFY(!mRepo.index().isTracked("fail.req"));

  const git_error *error = git_error_last();
  QVERIFY(error && QString(error->message).contains("fail.req"));
}

TEST_MAIN(TestFilterProcess)

#include "filterProcess.moc"
