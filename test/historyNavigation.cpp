//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "git/Commit.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "git/Signature.h"
#include "git/TagRef.h"
#include "ui/CommitList.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QFile>

using namespace QTest;

class TestHistoryNavigation : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();

  void parents();
  void children();
  void uncommittedChanges();
  void mergeBase();
  void selectedLine();
  void landmarks();

private:
  git::Commit commitFile(const QString &name, const QString &content);
  void select(const git::Commit &commit);
  git::Commit selected() const;

  Test::ScratchRepository mScratch;
  git::Repository mRepo;
  MainWindow *mWindow = nullptr;
  RepoView *mView = nullptr;
  CommitList *mList = nullptr;

  // A - B - M (main)
  //  \     /
  //   C --- (side)
  git::Commit mA, mB, mC, mM;
};

git::Commit TestHistoryNavigation::commitFile(const QString &name,
                                              const QString &content) {
  QFile file(mRepo.workdir().filePath(name));
  if (!file.open(QFile::WriteOnly | QFile::Truncate))
    return git::Commit();
  file.write(content.toUtf8());
  file.close();

  mRepo.index().setStaged({name}, true);
  return mRepo.commit(content);
}

void TestHistoryNavigation::initTestCase() {
  mRepo = mScratch;
  mA = commitFile("a.txt", "A");
  QVERIFY(mA.isValid());

  // The side branch, then the main branch and the merge.
  mC = commitFile("c.txt", "C");
  QVERIFY(mC.isValid());
  QVERIFY(mRepo.createBranch("side", mC).isValid());
  QVERIFY(mA.reset(GIT_RESET_HARD));
  mB = commitFile("b.txt", "B");
  QVERIFY(mB.isValid());
  mM = mRepo.commit("M", mC.annotatedCommit());
  QVERIFY(mM.isValid());
  QCOMPARE(mM.parents(), QList<git::Commit>({mB, mC}));

  mWindow = new MainWindow(mRepo);
  mWindow->show();
  QVERIFY(qWaitForWindowExposed(mWindow));
  mView = mWindow->currentView();
  QTRY_VERIFY_WITH_TIMEOUT(!mView->isLoading(), 30000);
  mList = mView->findChild<CommitList *>();
  QVERIFY(mList);
}

void TestHistoryNavigation::cleanupTestCase() {
  QTRY_VERIFY_WITH_TIMEOUT(!mView->isLoading(), 30000);
  delete mWindow;
}

void TestHistoryNavigation::select(const git::Commit &commit) {
  QVERIFY(mList->selectRange(commit.id().toString()));
  QCOMPARE(selected(), commit);
}

git::Commit TestHistoryNavigation::selected() const {
  QList<git::Commit> commits = mList->selectedCommits();
  return commits.size() == 1 ? commits.first() : git::Commit();
}

void TestHistoryNavigation::parents() {
  // The first parent follows the main line.
  select(mM);
  QVERIFY(mView->selectParent());
  QCOMPARE(selected(), mB);
  QVERIFY(mView->selectParent());
  QCOMPARE(selected(), mA);

  // There is nothing before the root.
  QVERIFY(!mView->selectParent());
  QCOMPARE(selected(), mA);

  // The second parent enters the merged branch.
  select(mM);
  QVERIFY(mView->selectParent(1));
  QCOMPARE(selected(), mC);
  QVERIFY(!mView->selectParent(1));
}

void TestHistoryNavigation::children() {
  QCOMPARE(mList->childCommits(mB), QList<git::Commit>({mM}));
  QCOMPARE(mList->childCommits(mC), QList<git::Commit>({mM}));

  QList<git::Commit> children = mList->childCommits(mA);
  QCOMPARE(children.size(), 2);
  QVERIFY(children.contains(mB));
  QVERIFY(children.contains(mC));

  // The commit details link them.
  QCOMPARE(mView->childCommits(mA), children);

  // A single child is selected right away.
  select(mC);
  QVERIFY(mView->selectChild());
  QCOMPARE(selected(), mM);
}

// The uncommitted changes are the child of HEAD.
void TestHistoryNavigation::uncommittedChanges() {
  QFile file(mRepo.workdir().filePath("dirty.txt"));
  QVERIFY(file.open(QFile::WriteOnly));
  file.write("dirty");
  file.close();
  Test::refresh(mView);
  QTRY_VERIFY_WITH_TIMEOUT(!mView->isLoading(), 30000);

  QCOMPARE(mList->childCommits(mM), QList<git::Commit>({git::Commit()}));

  select(mM);
  QVERIFY(mView->selectChild());
  QVERIFY(mList->selectedCommits().isEmpty()); // the status row
  QVERIFY(!mView->selectChild());

  QVERIFY(mView->selectParent());
  QCOMPARE(selected(), mM);
}

// Where the lines of the history meet.
void TestHistoryNavigation::mergeBase() {
  select(mM);
  QVERIFY(mList->selectMergeBase(mB, mC));
  QCOMPARE(selected(), mA);

  // An ancestor is the merge base itself.
  QVERIFY(mList->selectMergeBase(mM, mB));
  QCOMPARE(selected(), mB);
}

// The graph marks the line of first parents through the selection.
void TestHistoryNavigation::selectedLine() {
  select(mM);
  QVERIFY(mList->isOnSelectedLine(mM));
  QVERIFY(mList->isOnSelectedLine(mB));
  QVERIFY(mList->isOnSelectedLine(mA));
  QVERIFY(!mList->isOnSelectedLine(mC));

  // A merged branch ends where it is merged.
  select(mC);
  QVERIFY(mList->isOnSelectedLine(mC));
  QVERIFY(mList->isOnSelectedLine(mA));
  QVERIFY(!mList->isOnSelectedLine(mM));
  QVERIFY(!mList->isOnSelectedLine(mB));

  // The line doesn't continue up where the history branches off, and a
  // single commit is no line.
  select(mA);
  QVERIFY(!mList->isOnSelectedLine(mA));
  QVERIFY(!mList->isOnSelectedLine(mB));
  QVERIFY(!mList->isOnSelectedLine(mC));

  // It continues up along a single child.
  select(mB);
  QVERIFY(mList->isOnSelectedLine(mM));
  QVERIFY(mList->isOnSelectedLine(mA));
}

// Jumps to merges, tags and dates.
void TestHistoryNavigation::landmarks() {
  select(mA);
  QVERIFY(mView->selectMerge(false));
  QCOMPARE(selected(), mM);
  QVERIFY(!mView->selectMerge(false));
  QVERIFY(!mView->selectMerge(true));

  // Older tags load more rows if needed.
  QVERIFY(mRepo.createTag(mC, "v1").isValid());
  select(mM);
  QVERIFY(mView->selectTag(true));
  QCOMPARE(selected(), mC);
  QVERIFY(!mView->selectTag(true));

  // The newest commit at or before the date.
  QVERIFY(mView->selectDate(QDateTime::currentDateTime().addDays(1)));
  QCOMPARE(selected(), mM);
  QVERIFY(!mView->selectDate(mA.committer().date().addDays(-1)));
}

TEST_MAIN(TestHistoryNavigation)

#include "historyNavigation.moc"
