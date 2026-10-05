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
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QTimer>
#include <QScopeGuard>
#include <QSplitter>

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
  void panesKeepTheirSize();
  void branchNavigation();
  void childGoesBack();
  void childChoices();

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

// The details of a commit with long links to its parents and children
// widened the view, which shrank the sidebar whenever such a commit was
// selected.
void TestHistoryNavigation::panesKeepTheirSize() {
  mWindow->resize(1200, 800);
  select(mM);
  QTest::qWait(100);

  auto sizes = [this] {
    QList<QList<int>> result;
    for (QSplitter *splitter : mWindow->findChildren<QSplitter *>()) {
      if (splitter->isVisible())
        result.append(splitter->sizes());
    }
    return result;
  };

  QList<QList<int>> before = sizes();
  for (const git::Commit &commit : {mA, mB, mC, mM}) {
    select(commit);
    QTest::qWait(100);
    QCOMPARE(sizes(), before);
  }
}

// The arrow keys skip the commits of other branches in this mode.
void TestHistoryNavigation::branchNavigation() {
  // The mode is a setting of the user.
  bool enabled = CommitList::branchNavigation();
  CommitList::setBranchNavigation(true);
  auto restore = qScopeGuard([enabled] {
    CommitList::setBranchNavigation(enabled);
  });

  mList->setFocus();
  select(mM);
  keyClick(mList, Qt::Key_Down);
  QCOMPARE(selected(), mB);
  keyClick(mList, Qt::Key_Down);
  QCOMPARE(selected(), mA); // not mC of the merged branch
  keyClick(mList, Qt::Key_Down);
  QCOMPARE(selected(), mA); // the end of the branch

  keyClick(mList, Qt::Key_Up);
  QCOMPARE(selected(), mB);
  keyClick(mList, Qt::Key_Up);
  QCOMPARE(selected(), mM);

  // From a commit of another branch to the next one of this branch.
  select(mC);
  keyClick(mList, Qt::Key_Down);
  QCOMPARE(selected(), mA);

  // The shortcuts of the list do the same.
  select(mM);
  mList->selectCommitRelative(1);
  QCOMPARE(selected(), mB);
  mList->selectCommitRelative(-1);
  QCOMPARE(selected(), mM);

  // Without the mode, every row is visited.
  CommitList::setBranchNavigation(false);
  select(mB);
  int row = mList->currentIndex().row();
  keyClick(mList, Qt::Key_Down);
  QCOMPARE(mList->currentIndex().row(), row + 1);
}

// Child goes back the way Parent came, without asking.
void TestHistoryNavigation::childGoesBack() {
  select(mB);
  QVERIFY(mView->selectParent());
  QCOMPARE(selected(), mA);
  QVERIFY(mView->selectChild());
  QCOMPARE(selected(), mB);

  select(mC);
  QVERIFY(mView->selectParent());
  QCOMPARE(selected(), mA);
  QVERIFY(mView->selectChild());
  QCOMPARE(selected(), mC);
}

// The child on the current branch comes first, merges come last.
void TestHistoryNavigation::childChoices() {
  struct Item {
    QString text;
    bool bold;
    bool section;
  };

  // Read the menu, then take the first child with Enter.
  QList<Item> items;
  auto readMenu = [&items] {
    QTimer::singleShot(200, [&items] {
      QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      for (QAction *action : menu->actions())
        items.append({action->text(), action->font().bold(),
                      action->isSeparator()});
      QTest::keyClick(menu, Qt::Key_Return);
    });
  };

  select(mA);
  readMenu();
  QVERIFY(mView->selectChild(true));
  QCOMPARE(items.size(), 2);
  QVERIFY2(items.at(0).text.startsWith(mB.shortId()),
           qPrintable(items.at(0).text));
  QVERIFY(items.at(0).bold); // the current branch
  QVERIFY(items.at(1).text.startsWith(mC.shortId()));
  QVERIFY(!items.at(1).bold);
  QCOMPARE(selected(), mB);

  // M merges C.
  items.clear();
  select(mC);
  readMenu();
  QVERIFY(mView->selectChild(true));
  QCOMPARE(items.size(), 2);
  QVERIFY(items.at(0).section);
  QVERIFY(items.at(1).text.startsWith(mM.shortId()));
  QCOMPARE(selected(), mM);
}

TEST_MAIN(TestHistoryNavigation)

#include "historyNavigation.moc"
