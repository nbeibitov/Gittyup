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
#include "ui/CommitList.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "log/LogEntry.h"
#include "ui/TreeView.h"
#include <QSignalSpy>

using namespace Test;

// A change in the working directory updates the status row in place instead
// of walking the whole history again.
class TestCommitListStatus : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void workdirChangeKeepsRows();
  void headChangeRebuildsRows();
  void logStaysOpen();
  void slowStatusInLog();
  void cleanupTestCase();

private:
  void writeFile(const QString &name, const QString &content);
  bool waitForStatus();
  int unstagedCount() const;

  ScratchRepository mRepo;
  MainWindow *mWindow = nullptr;
  RepoView *mView = nullptr;
  CommitList *mCommits = nullptr;
};

void TestCommitListStatus::writeFile(const QString &name,
                                     const QString &content) {
  QFile file(mRepo->workdir().filePath(name));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  file.write(content.toUtf8());
}

bool TestCommitListStatus::waitForStatus() {
  QSignalSpy spy(mView, &RepoView::statusChanged);
  emit mRepo->notifier()->workdirChanged();
  return spy.wait(10000);
}

int TestCommitListStatus::unstagedCount() const {
  auto files = mView->findChild<TreeView *>("Unstaged");
  return files ? files->model()->rowCount() : -1;
}

void TestCommitListStatus::initTestCase() {
  writeFile("a.txt", "a\n");
  mRepo->index().setStaged({"a.txt"}, true);
  QVERIFY(mRepo->commit("A").isValid());
  writeFile("b.txt", "b\n");
  mRepo->index().setStaged({"b.txt"}, true);
  QVERIFY(mRepo->commit("B").isValid());

  writeFile("a.txt", "changed\n");

  mWindow = new MainWindow(mRepo);
  mWindow->show();
  QVERIFY(QTest::qWaitForWindowActive(mWindow));
  mView = mWindow->currentView();
  mCommits = mView->findChild<CommitList *>();
  QVERIFY(mCommits);

  refresh(mView);
  QTRY_COMPARE(unstagedCount(), 1);
}

void TestCommitListStatus::workdirChangeKeepsRows() {
  QSignalSpy resets(mCommits->model(), &QAbstractItemModel::modelReset);
  int rows = mCommits->model()->rowCount();

  // Another dirty file: the rows stay, the status and the diff are updated.
  writeFile("c.txt", "c\n");
  QVERIFY(waitForStatus());
  QCOMPARE(resets.count(), 0);
  QCOMPARE(mCommits->model()->rowCount(), rows);
  QCOMPARE(mCommits->status().count(), 2);
  QTRY_COMPARE(unstagedCount(), 2);
}

void TestCommitListStatus::headChangeRebuildsRows() {
  QSignalSpy resets(mCommits->model(), &QAbstractItemModel::modelReset);
  int rows = mCommits->model()->rowCount();

  // HEAD moves without a notification (like a commit on the command line).
  mRepo->notifier()->blockSignals(true);
  mRepo->index().setStaged({"c.txt"}, true);
  bool committed = mRepo->commit("C").isValid();
  mRepo->notifier()->blockSignals(false);
  QVERIFY(committed);
  QVERIFY(waitForStatus());
  QTRY_VERIFY(resets.count() > 0);
  QTRY_COMPARE(mCommits->model()->rowCount(), rows + 1);
}

void TestCommitListStatus::logStaysOpen() {
  if (mView->isLogVisible())
    mView->toggleLog();
  QVERIFY(!mView->isLogVisible());

  // Opened by the user: new entries don't hide it again.
  mView->toggleLog();
  QVERIFY(mView->isLogVisible());
  mView->addLogEntry("text", "title");
  QTest::qWait(2500);
  QVERIFY(mView->isLogVisible());

  // A repository opened later shows the log right away. (Two windows of
  // the same repository would share its notifier.)
  {
    ScratchRepository other;
    MainWindow window(other);
    QVERIFY(window.currentView()->isLogVisible());
  }

  mView->toggleLog();
  QVERIFY(!mView->isLogVisible());
  ScratchRepository other;
  MainWindow window(other);
  QVERIFY(!window.currentView()->isLogVisible());
}

void TestCommitListStatus::slowStatusInLog() {
  LogEntry *root =
      qobject_cast<LogEntry *>(mView->addLogEntry("text", "title")->parent());
  QVERIFY(root);
  int count = root->entries().size();

  // Fast checks are not logged.
  emit mCommits->statusProgress(10, 100, 1000);
  emit mCommits->statusChecked(1, 100, 1500);
  QCOMPARE(root->entries().size(), count);

  // Slow checks show their progress in one entry and end with the result.
  emit mCommits->statusProgress(1000, 160000, 4000);
  QCOMPARE(root->entries().size(), count + 1);
  LogEntry *entry = root->entries().last();
  QCOMPARE(entry->title(), QString("Status"));
  QVERIFY2(entry->text().contains("160"), qPrintable(entry->text()));

  emit mCommits->statusProgress(2000, 160000, 5000);
  QCOMPARE(root->entries().size(), count + 1);
  QVERIFY2(entry->text().contains("5 s"), qPrintable(entry->text()));

  emit mCommits->statusChecked(3, 160000, 60000);
  QCOMPARE(root->entries().size(), count + 1);
  QVERIFY2(entry->text().contains("1 min 0 s"), qPrintable(entry->text()));
  QCOMPARE(entry->entries().size(), 1); // hint about the index

  // The next slow check gets its own entry.
  emit mCommits->statusProgress(10, 100, 3500);
  QCOMPARE(root->entries().size(), count + 2);
  emit mCommits->statusChecked(0, 100, 3600);
}

void TestCommitListStatus::cleanupTestCase() {
  mWindow->close();
  delete mWindow;
}

TEST_MAIN(TestCommitListStatus)

#include "commitListStatus.moc"
