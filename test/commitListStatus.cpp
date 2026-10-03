//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "git/Config.h"
#include "git/Index.h"
#include "ui/CommitList.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "log/LogEntry.h"
#include "ui/ReferenceView.h"
#include "ui/ReferenceWidget.h"
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
  void secondWindowOfRepository();
  void externalChanges();
  void stashesTab();
  void pathHistoryInBackground();
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
  return spy.wait(60000);
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

  // A very slow check refreshes the stat information of the index (unless
  // a slow check of the test itself already did within ten minutes).
  emit mCommits->statusChecked(3, 160000, 60000);
  QVERIFY2(entry->text().contains("1 min 0 s"), qPrintable(entry->text()));
  LogEntry *index = nullptr;
  for (LogEntry *child : root->entries()) {
    if (child->title() == "Index")
      index = child;
  }
  QVERIFY(index);

  // Wait for it, it replaces the index used by the following tests.
  QTRY_VERIFY_WITH_TIMEOUT(!index->text().startsWith("updating"), 60000);

  // The next slow check gets its own entry.
  count = root->entries().size();
  emit mCommits->statusProgress(10, 100, 3500);
  QCOMPARE(root->entries().size(), count + 1);
  emit mCommits->statusChecked(0, 100, 3600);
}

void TestCommitListStatus::secondWindowOfRepository() {
  // A second window of the same repository object shares its notifier. Its
  // connections must go away with it.
  {
    MainWindow window(mRepo);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
  }

  emit mRepo->notifier()->referenceUpdated(mRepo->head());
  emit mRepo->notifier()->indexChanged({"a.txt"}, false);
  QVERIFY(waitForStatus());
}

void TestCommitListStatus::externalChanges() {
  // Earlier tests changed the repository without notifications.
  mView->checkExternalChanges();
  QTest::qWait(100);
  QTRY_VERIFY_WITH_TIMEOUT(!mView->isLoading(), 60000);

  // Nothing changed: nothing is refreshed.
  QSignalSpy resets(mCommits->model(), &QAbstractItemModel::modelReset);
  QSignalSpy status(mView, &RepoView::statusChanged);
  mView->checkExternalChanges();
  QTest::qWait(500);
  QCOMPARE(resets.count(), 0);
  QCOMPARE(status.count(), 0);

  // Staged by another tool: the status is checked again.
  writeFile("d.txt", "d\n");
  mRepo->notifier()->blockSignals(true);
  mRepo->index().setStaged({"d.txt"}, true);
  mRepo->notifier()->blockSignals(false);
  mView->checkExternalChanges();
  QVERIFY(status.wait(60000));
  QCOMPARE(resets.count(), 0);

  // Committed by another tool: the history is loaded again.
  int rows = mCommits->model()->rowCount();
  mRepo->notifier()->blockSignals(true);
  bool committed = mRepo->commit("D").isValid();
  mRepo->notifier()->blockSignals(false);
  QVERIFY(committed);
  mView->checkExternalChanges();
  QTRY_VERIFY_WITH_TIMEOUT(resets.count() > 0, 60000);
  QTRY_COMPARE_WITH_TIMEOUT(mCommits->model()->rowCount(), rows + 1, 60000);
}

void TestCommitListStatus::stashesTab() {
  writeFile("a.txt", "first stash\n");
  QVERIFY(mRepo->stash("first").isValid());
  writeFile("a.txt", "second stash\n");
  QVERIFY(mRepo->stash("second").isValid());
  QList<git::Commit> stashes = mRepo->stashes();
  QCOMPARE(stashes.size(), 2);

  // The reference selector has a tab with each stash, newest first.
  auto refs = mView->findChild<ReferenceWidget *>();
  QVERIFY(refs);
  auto view = refs->findChild<ReferenceView *>();
  QVERIFY(view);
  QAbstractItemModel *model = view->model();
  QModelIndex tab;
  for (int i = 0; i < model->rowCount(); ++i) {
    if (model->index(i, 0).data().toString() == "Stashes")
      tab = model->index(i, 0);
  }
  QVERIFY(tab.isValid());
  QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(tab), 2, 60000);
  QModelIndex second = model->index(1, 0, tab);
  QVERIFY2(second.data().toString().startsWith("stash@{1}: "),
           qPrintable(second.data().toString()));
  QCOMPARE(second.data(ReferenceView::StashIndexRole).toInt(), 1);

  // Choosing it (pressing makes it current, releasing clicks) shows the
  // stashes in the commit list and selects it.
  view->setCurrentIndex(second);
  emit view->clicked(second);
  QTRY_COMPARE_WITH_TIMEOUT(mView->commits().size(), 1, 60000);
  QCOMPARE(mView->commits().first().id(), stashes.at(1).id());
}

void TestCommitListStatus::pathHistoryInBackground() {
  // Without the search index, the history of a path is found by walking
  // and diffing the commits.
  mRepo->appConfig().setValue("index.enable", false);

  // More changes of the path than one page of rows.
  mRepo->notifier()->blockSignals(true);
  for (int i = 0; i < 140; ++i) {
    QString name = (i % 2) ? "g.txt" : "f.txt";
    writeFile(name, QString::number(i));
    mRepo->index().setStaged({name}, true);
    QVERIFY2(mRepo->commit(QString::number(i)).isValid(),
             qPrintable(git::Repository::lastError()));
  }
  mRepo->notifier()->blockSignals(false);
  mView->refresh();

  // Show the history of HEAD again (the previous test chose the stashes).
  mView->selectReference(mRepo->head());

  // The path is applied through a queued connection.
  auto setPath = [this](const QString &path) {
    mView->setPathspec(path);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(!mView->isLoading(), 60000);
  };

  setPath("f.txt");
  QAbstractItemModel *model = mCommits->model();


  // More rows are loaded in the background: fetchMore() returns at once.
  if (model->canFetchMore(QModelIndex())) {
    int rows = model->rowCount();
    model->fetchMore(QModelIndex());
    QVERIFY(mView->isLoading());
    QCOMPARE(model->rowCount(), rows);
  }

  auto loadAll = [model, this] {
    if (model->canFetchMore(QModelIndex()))
      model->fetchMore(QModelIndex());
    return !mView->isLoading() && !model->canFetchMore(QModelIndex());
  };
  QTRY_VERIFY_WITH_TIMEOUT(loadAll(), 60000);
  QCOMPARE(model->rowCount(), 70);

  // Selecting an old commit still loads the rows it needs right away.
  setPath("g.txt");
  git::Commit oldest;
  for (git::Commit commit = mRepo->head().target(); commit.isValid();
       commit = commit.parents().value(0)) {
    if (commit.summary() == "1")
      oldest = commit;
  }
  QVERIFY(oldest.isValid());
  bool found = mCommits->selectRange(oldest.id().toString());
  QVERIFY2(found, qPrintable(QString("rows=%1 more=%2 loading=%3")
                                 .arg(mCommits->model()->rowCount())
                                 .arg(mCommits->model()->canFetchMore({}))
                                 .arg(mView->isLoading())));

  setPath(QString());
  mRepo->appConfig().setValue("index.enable", true);
}

void TestCommitListStatus::cleanupTestCase() {
  mWindow->close();
  delete mWindow;
}

TEST_MAIN(TestCommitListStatus)

#include "commitListStatus.moc"
