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
#include "git/Config.h"
#include "git/Hook.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QDir>
#include <QFile>
#include <QStandardPaths>

using namespace git;

class TestHook : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void dir();
  void missing();
  void runs();
  void fails();

  void preCommitStopsCommit();
  void preCommitStagesFiles();
  void commitMsgChangesMessage();
  void postCommit();
  void hooksPath();

private:
  void writeHook(const QString &name, const QString &script,
                 const QString &dir = QString());
  void writeFile(const QString &name, const QString &content);
  bool commit(const QString &message);

  QScopedPointer<Test::ScratchRepository> mScratch;
  git::Repository mRepo;
  MainWindow *mWindow = nullptr;
};

void TestHook::init() {
#ifdef Q_OS_WIN
  if (Command::bashPath().isEmpty())
    QSKIP("bash of Git for Windows is not installed");
#endif

  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;
  writeFile("file.txt", "first");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(mRepo.commit("first").isValid());
}

void TestHook::cleanup() {
  if (mWindow) {
    RepoView *view = mWindow->currentView();
    QTRY_VERIFY_WITH_TIMEOUT(!view->isLoading(), 30000);
    delete mWindow;
    mWindow = nullptr;
  }

  mRepo = git::Repository();
  mScratch.reset();
}

void TestHook::writeHook(const QString &name, const QString &script,
                         const QString &dir) {
  QString hooks = dir.isEmpty() ? Hook::dir(mRepo.dir().path()) : dir;
  QVERIFY(QDir().mkpath(hooks));

  // Unix line endings, even on Windows.
  QFile file(QDir(hooks).filePath(name));
  QVERIFY(file.open(QFile::WriteOnly));
  file.write("#!/bin/sh\n" + script.toUtf8() + "\n");
  file.close();
  QVERIFY(file.setPermissions(file.permissions() | QFile::ExeOwner));
}

void TestHook::writeFile(const QString &name, const QString &content) {
  QFile file(mRepo.workdir().filePath(name));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  file.write(content.toUtf8());
}

// Commit staged changes like the commit editor does.
bool TestHook::commit(const QString &message) {
  if (!mWindow) {
    mWindow = new MainWindow(mRepo);
    mWindow->show();
    if (!QTest::qWaitForWindowExposed(mWindow))
      return false;
  }

  return mWindow->currentView()->commit(message);
}

void TestHook::dir() {
  QString gitDir = mRepo.dir().path();
  QCOMPARE(QDir(Hook::dir(gitDir)), QDir(mRepo.dir().filePath("hooks")));

  // Relative to the working tree, like git.
  mRepo.gitConfig().setValue("core.hooksPath", QString("tools/hooks"));
  QCOMPARE(QDir(Hook::dir(gitDir)),
           QDir(mRepo.workdir().filePath("tools/hooks")));

  QString absolute = QDir::temp().filePath("gittyup-hooks");
  mRepo.gitConfig().setValue("core.hooksPath", absolute);
  QCOMPARE(QDir(Hook::dir(gitDir)), QDir(absolute));
}

void TestHook::missing() {
  Hook::Result result = Hook::run(mRepo.dir().path(), "pre-commit");
  QVERIFY(!result.found);
  QVERIFY(result.succeeded());
}

void TestHook::runs() {
  writeHook("test-hook", "echo \"args: $1 $2\"\n"
                         "echo \"env: $GITTYUP_TEST\"\n"
                         "read line\n"
                         "echo \"input: $line\"\n"
                         "pwd");

  QProcessEnvironment env;
  env.insert("GITTYUP_TEST", "value");
  Hook::Result result = Hook::run(mRepo.dir().path(), "test-hook",
                                  {"one", "two"}, "text\n", env);
  QVERIFY2(result.succeeded(), qPrintable(result.error + result.output));
  QVERIFY(result.found);
  QVERIFY2(result.output.contains("args: one two"), qPrintable(result.output));
  QVERIFY2(result.output.contains("env: value"), qPrintable(result.output));
  QVERIFY2(result.output.contains("input: text"), qPrintable(result.output));

  // Hooks run in the root of the working tree.
  QString name = mRepo.workdir().dirName();
  QVERIFY2(result.output.contains(name), qPrintable(result.output));
}

void TestHook::fails() {
  writeHook("test-hook", "echo broken\nexit 3");

  Hook::Result result = Hook::run(mRepo.dir().path(), "test-hook");
  QVERIFY(result.found);
  QVERIFY(!result.succeeded());
  QCOMPARE(result.exitCode, 3);
  QCOMPARE(result.output, QString("broken"));
}

void TestHook::preCommitStopsCommit() {
  writeHook("pre-commit", "echo 'lint failed'\nexit 1");

  git::Id before = mRepo.head().target().id();
  writeFile("file.txt", "second");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(!commit("second"));
  QCOMPARE(mRepo.head().target().id(), before);
}

// Formatters fix and stage files in pre-commit hooks, e.g. lint-staged.
void TestHook::preCommitStagesFiles() {
  if (QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("git is not installed");

  writeHook("pre-commit", "echo fixed > other.txt\ngit add other.txt");

  writeFile("file.txt", "second");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(commit("second"));

  git::Commit head = mRepo.head().target();
  QCOMPARE(head.message().trimmed(), QString("second"));
  QVERIFY(head.blob("other.txt").isValid());
}

// Gerrit adds a Change-Id in its commit-msg hook.
void TestHook::commitMsgChangesMessage() {
  writeHook("commit-msg", "printf '\\n\\nChange-Id: I1234\\n' >> \"$1\"");
  writeHook("prepare-commit-msg", "echo \"$2\" > \"$1.source\"");

  writeFile("file.txt", "second");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(commit("second"));

  QString message = mRepo.head().target().message();
  QVERIFY2(message.startsWith("second"), qPrintable(message));
  QVERIFY2(message.contains("Change-Id: I1234"), qPrintable(message));

  QFile source(mRepo.dir().filePath("COMMIT_EDITMSG.source"));
  QVERIFY(source.open(QFile::ReadOnly));
  QCOMPARE(source.readAll().trimmed(), QByteArray("message"));
}

void TestHook::postCommit() {
  writeHook("post-commit", "echo done > post-commit-ran");

  writeFile("file.txt", "second");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(commit("second"));
  QVERIFY(QFile::exists(mRepo.workdir().filePath("post-commit-ran")));
}

void TestHook::hooksPath() {
  mRepo.gitConfig().setValue("core.hooksPath", QString(".husky"));
  writeHook("pre-commit", "exit 1", mRepo.workdir().filePath(".husky"));

  writeFile("file.txt", "second");
  mRepo.index().setStaged({"file.txt"}, true);
  QVERIFY(!commit("second"));
}

TEST_MAIN(TestHook)

#include "hook.moc"
