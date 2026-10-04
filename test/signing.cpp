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
#include "git/Index.h"
#include "git/Reference.h"
#include "git/Signature.h"
#include "git/Signing.h"
#include "git/Tree.h"
#include "git2/errors.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

class TestSigning : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void notEnabled();
  void signsCommits();
  void signsAmends();
  void failsWithoutKey();

private:
  QString sshKeygen() const;
  bool runGit(const QStringList &args, QString *output = nullptr) const;
  git::Commit commitFile(const QString &content);

  QScopedPointer<Test::ScratchRepository> mScratch;
  git::Repository mRepo;
  QTemporaryDir mKeys;
  QString mKey;
};

QString TestSigning::sshKeygen() const {
  QString path = QStandardPaths::findExecutable("ssh-keygen");
#ifdef Q_OS_WIN
  if (path.isEmpty()) {
    QString git = git::Command::gitPath();
    if (!git.isEmpty()) {
      QDir dir = QFileInfo(git).dir();
      dir.cdUp();
      path = QStandardPaths::findExecutable("ssh-keygen",
                                            {dir.filePath("usr/bin")});
    }
  }
#endif
  return path;
}

bool TestSigning::runGit(const QStringList &args, QString *output) const {
  QString git = QStandardPaths::findExecutable("git");
  QProcess process;
  process.setWorkingDirectory(mRepo.workdir().path());
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(git, args);
  bool finished = process.waitForFinished(60000);
  if (output)
    *output = QString::fromUtf8(process.readAll()).trimmed();
  return finished && process.exitCode() == 0;
}

void TestSigning::init() {
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;

  // A key without a passphrase.
  QString keygen = sshKeygen();
  if (keygen.isEmpty())
    return;

  mKey = QDir(mKeys.path()).filePath("key");
  QFile::remove(mKey);
  QFile::remove(mKey + ".pub");
  QProcess process;
  process.start(keygen, {"-q", "-t", "ed25519", "-N", "", "-C", "test", "-f",
                         QDir::toNativeSeparators(mKey)});
  QVERIFY(process.waitForFinished(60000));
  QCOMPARE(process.exitCode(), 0);
  QVERIFY(QFile::exists(mKey));
}

void TestSigning::cleanup() {
  mRepo = git::Repository();
  mScratch.reset();
}

git::Commit TestSigning::commitFile(const QString &content) {
  QFile file(mRepo.workdir().filePath("file.txt"));
  if (!file.open(QFile::WriteOnly | QFile::Truncate))
    return git::Commit();
  file.write(content.toUtf8());
  file.close();

  mRepo.index().setStaged({"file.txt"}, true);
  return mRepo.commit(content);
}

void TestSigning::notEnabled() {
  QVERIFY(!git::Signing::isEnabled(mRepo.gitConfig()));
  git::Commit commit = commitFile("first");
  QVERIFY(commit.isValid());

  QString output;
  if (!QStandardPaths::findExecutable("git").isEmpty()) {
    QVERIFY(runGit({"cat-file", "commit", "HEAD"}, &output));
    QVERIFY2(!output.contains("gpgsig"), qPrintable(output));
  }
}

void TestSigning::signsCommits() {
  if (mKey.isEmpty() || QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("ssh-keygen or git is not installed");

  git::Config config = mRepo.gitConfig();
  config.setValue("commit.gpgSign", true);
  config.setValue("gpg.format", QString("ssh"));
  config.setValue("user.signingKey", mKey);

  QVERIFY(commitFile("first").isValid());
  git::Commit second = commitFile("second");
  QVERIFY(second.isValid());
  QCOMPARE(mRepo.head().target().id(), second.id());

  QString output;
  QVERIFY(runGit({"cat-file", "commit", "HEAD"}, &output));
  QVERIFY2(output.contains("gpgsig -----BEGIN SSH SIGNATURE-----"),
           qPrintable(output));

  // git verifies it with the public key.
  QString email = mRepo.defaultSignature().email();
  QFile pub(mKey + ".pub");
  QVERIFY(pub.open(QFile::ReadOnly));
  QString signers = QDir(mKeys.path()).filePath("allowed_signers");
  QFile allowed(signers);
  QVERIFY(allowed.open(QFile::WriteOnly));
  allowed.write(email.toUtf8() + " " + pub.readAll());
  allowed.close();

  QVERIFY2(runGit({"-c", "gpg.ssh.allowedSignersFile=" + signers,
                   "verify-commit", "HEAD"},
                  &output),
           qPrintable(output));

  // The reflog reads like git's.
  QVERIFY(runGit({"reflog", "-2", "--format=%gs"}, &output));
  QCOMPARE(output, QString("commit: second\ncommit (initial): first"));
}

void TestSigning::signsAmends() {
  if (mKey.isEmpty() || QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("ssh-keygen or git is not installed");

  git::Commit first = commitFile("first");
  QVERIFY(first.isValid());
  git::Commit second = commitFile("second");
  QVERIFY(second.isValid());

  git::Config config = mRepo.gitConfig();
  config.setValue("commit.gpgSign", true);
  config.setValue("gpg.format", QString("ssh"));
  config.setValue("user.signingKey", mKey);

  git::Signature author = second.author();
  QVERIFY(second.amend(author, author, "amended", git::Tree()));

  git::Commit head = mRepo.head().target();
  QCOMPARE(head.message(), QString("amended"));
  QCOMPARE(head.parents().first().id(), first.id());
  QCOMPARE(head.tree().Object::id(), second.tree().Object::id());

  QString output;
  QVERIFY(runGit({"cat-file", "commit", "HEAD"}, &output));
  QVERIFY2(output.contains("gpgsig -----BEGIN SSH SIGNATURE-----"),
           qPrintable(output));
  QVERIFY(runGit({"reflog", "-1", "--format=%gs"}, &output));
  QCOMPARE(output, QString("commit (amend): amended"));
}

void TestSigning::failsWithoutKey() {
  QVERIFY(commitFile("first").isValid());
  git::Id before = mRepo.head().target().id();

  git::Config config = mRepo.gitConfig();
  config.setValue("commit.gpgSign", true);
  config.setValue("gpg.format", QString("ssh"));

  QVERIFY(!commitFile("second").isValid());
  QCOMPARE(mRepo.head().target().id(), before);

  const git_error *error = git_error_last();
  QVERIFY(error && QString(error->message).contains("user.signingKey"));
}

TEST_MAIN(TestSigning)

#include "signing.moc"
