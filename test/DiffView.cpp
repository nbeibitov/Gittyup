#include "Test.h"
#include "conf/Settings.h"
#include "git/Command.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "ui/TreeView.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DiffView/HunkWidget.h"

#include <QLabel>
#include <QProcess>
#include <QScrollBar>
#include <QToolButton>

using namespace Test;
using namespace QTest;

namespace {

// Far more hunks than fit in the viewport, so lazy loading has to stop early.
const int kHunkCount = 60;

QMap<QString, QString> contents(bool manyFiles, const QString &state) {
  QMap<QString, QString> files;
  if (manyFiles) {
    for (int i = 0; i < kHunkCount; ++i) {
      QString name = QString("files/file%1.txt").arg(i, 3, 10, QChar('0'));
      files.insert(name, state + "\n");
    }
    return files;
  }

  // Enough unchanged lines between the edits to keep each one in its own hunk.
  QString text;
  for (int i = 0; i < kHunkCount; ++i) {
    for (int j = 0; j < 10; ++j)
      text += QString("line %1.%2\n").arg(i).arg(j);
    text += QString("%1 %2\n").arg(state).arg(i);
  }
  files.insert("files/hunks.txt", text);
  return files;
}

bool writeFiles(const QDir &workdir, const QMap<QString, QString> &files) {
  for (auto it = files.begin(); it != files.end(); ++it) {
    QFile file(workdir.filePath(it.key()));
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
      return false;
    file.write(it.value().toUtf8());
  }
  return true;
}

} // namespace

class TestDiffView : public QObject {
  Q_OBJECT

private slots:
  void loadsUntilScrollbarVisible_data();
  void loadsUntilScrollbarVisible();
  void conflicts();
};

void TestDiffView::conflicts() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Settings::instance()->setValue(Setting::Id::ShowChangedFilesAsList, false);

  ScratchRepository repo;
  QDir workdir = repo->workdir();
  auto run = [&](const QStringList &args) {
    QProcess process;
    process.setWorkingDirectory(workdir.path());
    process.start(git::Command::gitPath(), args);
    return process.waitForFinished(60000) &&
           process.exitStatus() == QProcess::NormalExit;
  };

  // Two blocks in a.txt, far enough apart, and one in b.txt.
  auto text = [](const QString &first, const QString &second) {
    QString result = first + "\n";
    for (int i = 0; i < 12; ++i)
      result += QString("line %1\n").arg(i);
    return result + second + "\n";
  };
  auto commit = [&](const QString &first, const QString &second) {
    QMap<QString, QString> files = {{"a.txt", text(first, second)},
                                    {"b.txt", first + "\n"}};
    return writeFiles(workdir, files) && run({"add", "."}) &&
           run({"commit", "-q", "-m", first});
  };

  QVERIFY(run({"config", "user.name", "Test"}));
  QVERIFY(run({"config", "user.email", "test@example.com"}));
  QVERIFY(run({"config", "core.autocrlf", "false"}));
  QVERIFY(commit("base", "base"));
  QVERIFY(run({"checkout", "-q", "-b", "other"}));
  QVERIFY(commit("theirs", "theirs 2"));
  QVERIFY(run({"checkout", "-q", "-"}));
  QVERIFY(commit("ours", "ours 2"));
  run({"merge", "-q", "other"}); // fails with the conflicts

  MainWindow window(repo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  window.resize(1200, 900);

  RepoView *repoView = window.currentView();
  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto bar = doubleTree->findChild<QWidget *>("ConflictBar");
  auto status = doubleTree->findChild<QLabel *>("ConflictStatus");
  auto next = doubleTree->findChild<QToolButton *>("ConflictNext");
  auto previous = doubleTree->findChild<QToolButton *>("ConflictPrevious");
  QVERIFY(bar && status && next && previous);
  QTRY_COMPARE_WITH_TIMEOUT(status->text(),
                            QString("Unresolved conflicts: 3, files: 2"),
                            30000);

  auto diffView = repoView->findChild<DiffView *>();
  auto shownHunks = [diffView] {
    return diffView->widget()->findChildren<HunkWidget *>();
  };
  auto readFile = [&](const QString &name) {
    QFile file(workdir.filePath(name));
    return file.open(QFile::ReadOnly) ? QString::fromUtf8(file.readAll())
                                      : QString();
  };

  // Next selects the file and goes through its blocks, then the next file.
  next->click();
  QCOMPARE(status->text(), QString("Conflict 1 of 3: a.txt"));
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 2, 10000);

  next->click();
  QCOMPARE(status->text(), QString("Conflict 2 of 3: a.txt"));

  next->click();
  QCOMPARE(status->text(), QString("Conflict 3 of 3: b.txt"));
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 1, 10000);

  // It wraps around at both ends.
  next->click();
  QCOMPARE(status->text(), QString("Conflict 1 of 3: a.txt"));
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 2, 10000);
  previous->click();
  QCOMPARE(status->text(), QString("Conflict 3 of 3: b.txt"));
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 1, 10000);

  // Use Both keeps both sides in order and moves on to the next conflict.
  HunkWidget *hunk = shownHunks().first();
  auto both = hunk->findChild<QToolButton *>("ConflictBoth");
  QVERIFY(both);
  both->click();
  auto save = hunk->findChild<QToolButton *>("ConflictSave");
  QVERIFY(save);
  save->click();
  QTRY_COMPARE_WITH_TIMEOUT(readFile("b.txt"), QString("ours\ntheirs\n"),
                            10000);
  QTRY_COMPARE_WITH_TIMEOUT(status->text(), QString("Conflict 2 of 2: a.txt"),
                            30000);
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 2, 10000);

  // Solving the other blocks removes the bar.
  hunk = shownHunks().first();
  hunk->findChild<QToolButton *>("ConflictTheirs")->click();
  hunk->findChild<QToolButton *>("ConflictSave")->click();
  QTRY_COMPARE_WITH_TIMEOUT(status->text(), QString("Conflict 1 of 1: a.txt"),
                            30000);
  QTRY_COMPARE_WITH_TIMEOUT(shownHunks().size(), 1, 10000);

  hunk = shownHunks().first();
  hunk->findChild<QToolButton *>("ConflictOurs")->click();
  hunk->findChild<QToolButton *>("ConflictSave")->click();
  QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 30000);
  QCOMPARE(readFile("a.txt"), text("theirs", "ours 2"));
}

void TestDiffView::loadsUntilScrollbarVisible_data() {
  QTest::addColumn<bool>("manyFiles");
  QTest::addColumn<bool>("committed");

  // Hunks of a single file are only loaded lazily for commit diffs; a working
  // tree diff always loads them all at once.
  QTest::newRow("many files, working tree") << true << false;
  QTest::newRow("many files, commit") << true << true;
  QTest::newRow("many hunks in one file, commit") << false << true;
}

void TestDiffView::loadsUntilScrollbarVisible() {
  QFETCH(bool, manyFiles);
  QFETCH(bool, committed);

  Settings::instance()->setValue(Setting::Id::ShowChangedFilesAsList, false);

  ScratchRepository repo;
  QDir workdir = repo->workdir();
  QVERIFY(workdir.mkpath("files"));

  auto before = contents(manyFiles, "before");
  QVERIFY(writeFiles(workdir, before));
  repo->index().setStaged(before.keys(), true);
  QVERIFY(repo->commit("initial"));

  QVERIFY(writeFiles(workdir, contents(manyFiles, "after")));
  if (committed) {
    repo->index().setStaged(before.keys(), true);
    QVERIFY(repo->commit("change"));
  }

  MainWindow window(repo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));

  // Tall enough that the first four hunks fit without a scrollbar.
  window.resize(1200, 2000);

  RepoView *repoView = window.currentView();
  if (committed)
    repoView->selectFirstCommit();

  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(unstagedTree);

  QAbstractItemModel *model = unstagedTree->model();
  QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 10000);

  // Select the folder so the diff view shows every file below it.
  QModelIndex folder = model->index(0, 0);
  QVERIFY(folder.isValid());
  unstagedTree->selectionModel()->select(folder, QItemSelectionModel::Select);

  auto diffView = repoView->findChild<DiffView *>();
  QVERIFY(diffView);
  QScrollBar *scrollBar = diffView->verticalScrollBar();
  auto hunks = [diffView] {
    return diffView->widget()->findChildren<HunkWidget *>();
  };

  // Without the follow-up fetch this stays at zero, since nothing scrolls or
  // resizes once the initial batch fits on screen.
  QTRY_VERIFY_WITH_TIMEOUT(scrollBar->maximum() > 0, 10000);

  int previousCount = -1;
  auto loadingSettled = [&] {
    int count = hunks().size();
    bool settled = count == previousCount;
    previousCount = count;
    return settled;
  };
  QTRY_VERIFY_WITH_TIMEOUT(loadingSettled(), 5000);

  // Once loading stops the scrollbar must still be there, with more than the
  // initial batch of four loaded behind it.
  const QList<HunkWidget *> loaded = hunks();
  QVERIFY(scrollBar->maximum() > 0);
  QVERIFY(loaded.size() > 4);

  // The initial batch of four must fit in the viewport, or this proves nothing.
  QWidget *fourth = loaded.at(3);
  QPoint bottomLeft(0, fourth->height());
  int bottom = fourth->mapTo(diffView->widget(), bottomLeft).y();
  QVERIFY2(bottom < diffView->viewport()->height(),
           "The first four hunks don't fit without a scrollbar");

  // It should stop once the scrollbar shows rather than load everything.
  QVERIFY(loaded.size() < kHunkCount);
}

TEST_MAIN(TestDiffView)

#include "DiffView.moc"
