//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "git/Index.h"
#include "git/IndexRefresh.h"
#include "git2.h"
#include "git2/sys/diff.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

using namespace git;

class TestIndexRefresh : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void refreshMakesStatusFast();
  void upToDate();
  void canceled();
  void locked();

private:
  void touchAll();
  int hashedByStatus() const;
  QByteArray index() const;

  static const int kFiles = 20;

  QScopedPointer<Test::ScratchRepository> mScratch;
  Repository mRepo;
};

void TestIndexRefresh::init() {
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;

  QStringList names;
  for (int i = 0; i < kFiles; ++i) {
    QString name = QString("file%1.txt").arg(i);
    QFile file(mRepo.workdir().filePath(name));
    QVERIFY(file.open(QFile::WriteOnly));
    file.write(QByteArray::number(i));
    names.append(name);
  }

  mRepo.index().setStaged(names, true);
  QVERIFY(mRepo.commit("files").isValid());
}

void TestIndexRefresh::cleanup() {
  mRepo = Repository();
  mScratch.reset();
}

// Change the modification time but not the content, like many tools do.
void TestIndexRefresh::touchAll() {
  QDateTime time = QDateTime::currentDateTime().addSecs(-3600);
  for (int i = 0; i < kFiles; ++i) {
    QFile file(mRepo.workdir().filePath(QString("file%1.txt").arg(i)));
    QVERIFY(file.open(QFile::ReadWrite));
    QVERIFY(file.setFileTime(time, QFileDevice::FileModificationTime));
  }
}

// Number of files a status check reads to compare their content.
int TestIndexRefresh::hashedByStatus() const {
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, mRepo.dir().path().toUtf8()))
    return -1;

  git_diff *diff = nullptr;
  int hashed = -1;
  if (!git_diff_index_to_workdir(&diff, repo, nullptr, nullptr)) {
    git_diff_perfdata perf = GIT_DIFF_PERFDATA_INIT;
    if (!git_diff_get_perfdata(&perf, diff))
      hashed = static_cast<int>(perf.oid_calculations);
    if (git_diff_num_deltas(diff)) // the content must be unchanged
      hashed = -2;
    git_diff_free(diff);
  }

  git_repository_free(repo);
  return hashed;
}

QByteArray TestIndexRefresh::index() const {
  QFile file(mRepo.dir().filePath("index"));
  return file.open(QFile::ReadOnly) ? file.readAll() : QByteArray();
}

void TestIndexRefresh::refreshMakesStatusFast() {
  touchAll();
  QCOMPARE(hashedByStatus(), kFiles);

  IndexRefresh::Result result = IndexRefresh::run(mRepo.dir().path());
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QVERIFY(result.written);
  QCOMPARE(result.hashed, kFiles);
  QVERIFY(!QFile::exists(mRepo.dir().filePath("index.lock")));
  QVERIFY(!QFile::exists(mRepo.dir().filePath("gittyup/index-refresh")));

  // The index is still valid for the repository and its users.
  QCOMPARE(hashedByStatus(), 0);
  mRepo.index().read();
  QVERIFY(!mRepo.status(mRepo.index(), nullptr).isValid()); // clean

  // git accepts the index too.
  QString git = QStandardPaths::findExecutable("git");
  if (!git.isEmpty()) {
    QProcess process;
    process.setWorkingDirectory(mRepo.workdir().path());
    process.start(git, {"status", "--porcelain"});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    QCOMPARE(process.readAllStandardOutput(), QByteArray());
  }
}

void TestIndexRefresh::upToDate() {
  QByteArray before = index();
  IndexRefresh::Result result = IndexRefresh::run(mRepo.dir().path());
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QVERIFY(!result.written);
  QCOMPARE(index(), before);
}

void TestIndexRefresh::canceled() {
  touchAll();
  QByteArray before = index();

  std::atomic<bool> canceled = true;
  IndexRefresh::Result result =
      IndexRefresh::run(mRepo.dir().path(), &canceled);
  QVERIFY(!result.written);
  QVERIFY(!result.error.isEmpty());
  QCOMPARE(index(), before);
}

void TestIndexRefresh::locked() {
  touchAll();
  QByteArray before = index();

  // Someone else (e.g. git) is writing the index.
  QFile lock(mRepo.dir().filePath("index.lock"));
  QVERIFY(lock.open(QFile::WriteOnly));
  lock.close();

  IndexRefresh::Result result = IndexRefresh::run(mRepo.dir().path());
  QVERIFY(!result.written);
  QVERIFY(!result.error.isEmpty());
  QCOMPARE(index(), before);
  QVERIFY(QFile::exists(mRepo.dir().filePath("index.lock")));
  QVERIFY(QFile::remove(mRepo.dir().filePath("index.lock")));
}

TEST_MAIN(TestIndexRefresh)

#include "indexRefresh.moc"
