//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "dialogs/GitMessageDialog.h"
#include "dialogs/InteractiveRebaseDialog.h"
#include "dialogs/RebaseTodoDialog.h"
#include "git/Branch.h"
#include "git/Command.h"
#include "git/Commit.h"
#include "git/InteractiveRebase.h"
#include "git/RebaseState.h"
#include "git/Reference.h"
#include "ui/GitEditor.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeWidget>

using namespace git;

class TestRebaseMerges : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void todoDialog();
  void todoDialogValidation();
  void todoDialogText();
  void todoDialogUi();
  void messageDialog();
  void containsMerge();

  void rebaseWithDialogs();
  void cancelKeepsBranch();
  void conflictStopsAndAborts();
  void rebaseFromCommit();

private:
  bool git(const QStringList &args, QString *output = nullptr);
  void commitFile(const QString &name, const QString &content,
                  const QString &message);
  void createMergeHistory(bool conflict = false);
  RepoView *openView();
  template <typename T> T *visibleDialog() const;
  int rowOf(RebaseTodoDialog *dialog, const QString &description) const;
  QStringList firstParentSummaries(int count) const;

  QScopedPointer<Test::ScratchRepository> mScratch;
  git::Repository mRepo;
  MainWindow *mWindow = nullptr;
  QString mOther;
};

namespace {

const QString kTodo =
    "label onto\n"
    "\n"
    "# Branch side\n"
    "reset onto\n"
    "pick 1111111 S\n"
    "label side\n"
    "\n"
    "reset onto\n"
    "pick 2222222 A\n"
    "merge -C 3333333 side # Merge side\n"
    "pick 4444444 B\n"
    "\n"
    "# Rebase 0000000..4444444 onto 5555555 (8 commands)\n"
    "#\n"
    "# Commands:\n"
    "# p, pick <commit> = use commit\n";

} // namespace

void TestRebaseMerges::init() {
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;
}

void TestRebaseMerges::cleanup() {
  if (mWindow) {
    RepoView *view = mWindow->currentView();
    QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!view->isLoading(), 30000);
    delete mWindow;
    mWindow = nullptr;
  }

  mRepo = git::Repository();
  mScratch.reset();
}

bool TestRebaseMerges::git(const QStringList &args, QString *output) {
  QProcess process;
  process.setWorkingDirectory(mRepo.workdir().path());
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("GIT_EDITOR", "true");
  process.setProcessEnvironment(env);
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(Command::gitPath(), args);
  if (!process.waitForFinished(60000))
    return false;

  QString text = QString::fromUtf8(process.readAll());
  if (output)
    *output = text;
  if (process.exitCode())
    qWarning().noquote() << args.join(' ') << text;
  return process.exitStatus() == QProcess::NormalExit && !process.exitCode();
}

void TestRebaseMerges::commitFile(const QString &name, const QString &content,
                                  const QString &message) {
  QFile file(mRepo.workdir().filePath(name));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  file.write(content.toUtf8());
  file.close();
  QVERIFY(git({"add", name}));
  QVERIFY(git({"commit", "-q", "-m", message}));
}

// base - A - merge(side: S) - B on the current branch,
// base - O on 'other'. O conflicts with A if conflict is true.
void TestRebaseMerges::createMergeHistory(bool conflict) {
  QVERIFY(git({"config", "user.name", "Test"}));
  QVERIFY(git({"config", "user.email", "test@example.com"}));
  QVERIFY(git({"config", "core.autocrlf", "false"}));
  commitFile("f.txt", "base\n", "base");

  QVERIFY(git({"checkout", "-q", "-b", "other"}));
  commitFile(conflict ? "a.txt" : "o.txt", "other\n", "O");
  QString output;
  QVERIFY(git({"rev-parse", "HEAD"}, &output));
  mOther = output.trimmed();

  QVERIFY(git({"checkout", "-q", "-"}));
  QVERIFY(git({"checkout", "-q", "-b", "side"}));
  commitFile("s.txt", "s\n", "S");
  QVERIFY(git({"checkout", "-q", "-"}));
  commitFile("a.txt", "a\n", "A");
  QVERIFY(git({"merge", "-q", "--no-ff", "-m", "Merge side", "side"}));
  commitFile("b.txt", "b\n", "B");
}

RepoView *TestRebaseMerges::openView() {
  mWindow = new MainWindow(mRepo);
  mWindow->show();
  if (!QTest::qWaitForWindowExposed(mWindow))
    return nullptr;

  RepoView *view = mWindow->currentView();
  QDeadlineTimer deadline(30000);
  while (view->isLoading() && !deadline.hasExpired())
    QTest::qWait(50);
  return view->isLoading() ? nullptr : view;
}

template <typename T> T *TestRebaseMerges::visibleDialog() const {
  if (!mWindow)
    return nullptr;
  for (T *dialog : mWindow->findChildren<T *>()) {
    if (dialog->isVisible())
      return dialog;
  }
  return nullptr;
}

int TestRebaseMerges::rowOf(RebaseTodoDialog *dialog,
                            const QString &description) const {
  QTreeWidget *list = dialog->findChild<QTreeWidget *>("RebaseTodoList");
  for (int row = 0; row < list->topLevelItemCount(); ++row) {
    if (list->topLevelItem(row)->text(2) == description)
      return row;
  }
  return -1;
}

QStringList TestRebaseMerges::firstParentSummaries(int count) const {
  QStringList result;
  Commit commit = mRepo.head().target();
  while (commit.isValid() && result.size() < count) {
    result.append(commit.summary());
    QList<Commit> parents = commit.parents();
    commit = parents.isEmpty() ? Commit() : parents.first();
  }
  return result;
}

void TestRebaseMerges::todoDialog() {
  RebaseTodoDialog dialog(kTodo);
  QCOMPARE(dialog.rowCount(), 8);
  QCOMPARE(dialog.command(0), QString("label"));
  QCOMPARE(dialog.command(2), QString("pick"));
  QCOMPARE(dialog.command(6), QString("merge"));

  // Commits take all actions, merges pick, reword and drop; structure rows
  // none.
  QVERIFY(dialog.setCommand(2, "reword"));
  QVERIFY(dialog.setCommand(7, "squash"));
  QVERIFY(dialog.setCommand(6, "reword"));
  QCOMPARE(dialog.command(6), QString("reword"));
  QVERIFY(!dialog.setCommand(6, "squash"));
  QVERIFY(!dialog.setCommand(0, "pick"));
  QVERIFY(dialog.validate().isEmpty());

  QString todo = dialog.todo();
  QVERIFY2(todo.contains("reword 1111111 S\n"), qPrintable(todo));
  QVERIFY2(todo.contains("merge -c 3333333 side # Merge side\n"),
           qPrintable(todo));
  QVERIFY2(todo.contains("squash 4444444 B\n"), qPrintable(todo));
  QVERIFY2(!todo.contains("Commands"), qPrintable(todo));

  // Abbreviations and fixup options.
  RebaseTodoDialog short_("p 1111111 S\nf -C 2222222 A\n");
  QCOMPARE(short_.command(0), QString("pick"));
  QCOMPARE(short_.command(1), QString("fixup"));
  QCOMPARE(short_.todo(), QString("pick 1111111 S\nfixup -C 2222222 A\n"));
  QVERIFY(short_.setCommand(1, "squash"));
  QCOMPARE(short_.todo(), QString("pick 1111111 S\nsquash 2222222 A\n"));

  short_.moveRow(1, -1);
  QCOMPARE(short_.todo(), QString("squash 2222222 A\npick 1111111 S\n"));
}

void TestRebaseMerges::todoDialogValidation() {
  RebaseTodoDialog dialog(kTodo);
  // The first commit after 'reset onto' has nothing to squash into.
  QVERIFY(dialog.setCommand(2, "squash"));
  QVERIFY(!dialog.validate().isEmpty());
  QVERIFY(dialog.setCommand(2, "pick"));
  QVERIFY(dialog.setCommand(5, "fixup"));
  QVERIFY(!dialog.validate().isEmpty());

  // After a merge it can.
  QVERIFY(dialog.setCommand(5, "pick"));
  QVERIFY(dialog.setCommand(7, "fixup"));
  QVERIFY(dialog.validate().isEmpty());
}

void TestRebaseMerges::todoDialogText() {
  RebaseTodoDialog dialog("pick 1111111 S\npick 2222222 A\n");
  QTabWidget *tabs = dialog.findChild<QTabWidget *>();
  QPlainTextEdit *text = dialog.findChild<QPlainTextEdit *>("RebaseTodoText");
  QVERIFY(tabs && text);

  tabs->setCurrentIndex(1);
  QCOMPARE(text->toPlainText(), QString("pick 1111111 S\npick 2222222 A\n"));
  text->setPlainText("pick 2222222 A\nexec make\npick 1111111 S\n");
  QCOMPARE(dialog.todo(), text->toPlainText());

  tabs->setCurrentIndex(0);
  QCOMPARE(dialog.rowCount(), 3);
  QCOMPARE(dialog.command(1), QString("exec"));
  QCOMPARE(dialog.todo(),
           QString("pick 2222222 A\nexec make\npick 1111111 S\n"));
}

void TestRebaseMerges::todoDialogUi() {
  RebaseTodoDialog dialog(kTodo);
  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  QTreeWidget *list = dialog.findChild<QTreeWidget *>("RebaseTodoList");
  QComboBox *combo = dialog.findChild<QComboBox *>("RebaseTodoAction");
  QVERIFY(list && combo);

  auto click = [list](int row) {
    QRect rect = list->visualItemRect(list->topLevelItem(row));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, rect.center());
  };

  // Commits take keys and the combo box.
  click(5);
  QTest::keyClick(list, Qt::Key_D);
  QCOMPARE(dialog.command(5), QString("drop"));
  QCOMPARE(combo->currentData().toString(), QString("drop"));
  int reword = combo->findData("reword");
  combo->setCurrentIndex(reword);
  emit combo->activated(reword);
  QCOMPARE(dialog.command(5), QString("reword"));

  // The combo box follows the merge and offers only what applies.
  click(6);
  QVERIFY(combo->isEnabled());
  QCOMPARE(combo->currentData().toString(), QString("pick"));
  auto *model = qobject_cast<QStandardItemModel *>(combo->model());
  QVERIFY(model);
  QVERIFY(!model->item(combo->findData("squash"))->isEnabled());
  QVERIFY(model->item(combo->findData("drop"))->isEnabled());

  QTest::keyClick(list, Qt::Key_D);
  QCOMPARE(dialog.command(6), QString("drop"));
  QVERIFY2(!dialog.todo().contains("merge"), qPrintable(dialog.todo()));
  QTest::keyClick(list, Qt::Key_P);
  QCOMPARE(dialog.command(6), QString("merge"));
  QVERIFY(dialog.todo().contains("merge -C 3333333 side # Merge side\n"));

  // Structure rows can't be changed here.
  click(1);
  QVERIFY(!combo->isEnabled());
  QTest::keyClick(list, Qt::Key_D);
  QCOMPARE(dialog.command(1), QString("reset"));
}

void TestRebaseMerges::messageDialog() {
  QString text = "A\n\nBody\n\n# Please enter the commit message.\n"
                 "# Lines starting with '#' will be ignored.\n";
  GitMessageDialog dialog(text, '#');
  QCOMPARE(dialog.message(), QString("A\n\nBody"));

  dialog.setMessage("Reworded\n");
  QCOMPARE(dialog.text(),
           QString("Reworded\n\n# Please enter the commit message.\n"
                   "# Lines starting with '#' will be ignored.\n"));

  // Other comment characters (core.commentChar).
  GitMessageDialog semicolon("Msg\n; hint\n", ';');
  QCOMPARE(semicolon.message(), QString("Msg"));
}

void TestRebaseMerges::containsMerge() {
  if (Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  createMergeHistory();
  Commit head = mRepo.head().target();
  QVERIFY(!InteractiveRebase::containsMerge(mRepo, head.parents().first()));
  QVERIFY(InteractiveRebase::containsMerge(
      mRepo, mRepo.lookupCommit(mOther).parents().first()));
}

void TestRebaseMerges::rebaseWithDialogs() {
  if (Command::gitPath().isEmpty())
    QSKIP("git is not installed");
  if (GitEditor::helperPath().isEmpty())
    QSKIP("gittyup-editor is not built");

  createMergeHistory();
  RepoView *view = openView();
  QVERIFY(view);

  Reference other = mRepo.lookupRef("refs/heads/other");
  QVERIFY(other.isValid());
  view->interactiveRebaseOnto(other);
  QVERIFY(view->isRunningGit());

  // Git asks for the todo list.
  RebaseTodoDialog *todo = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((todo = visibleDialog<RebaseTodoDialog>()), 30000);
  int row = rowOf(todo, "A");
  QVERIFY(row >= 0);
  QVERIFY(todo->setCommand(row, "reword"));
  todo->accept();

  // and for the new message of A.
  GitMessageDialog *message = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((message = visibleDialog<GitMessageDialog>()),
                           30000);
  QCOMPARE(message->message(), QString("A"));
  message->setMessage("A reworded");
  message->accept();

  QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 60000);
  QVERIFY(!RebaseState::read(mRepo).isValid());

  // B - merge - A' - O: the merge is kept, A reworded, all on top of O.
  QCOMPARE(firstParentSummaries(4),
           QStringList({"B", "Merge side", "A reworded", "O"}));
  Commit merge = mRepo.head().target().parents().first();
  QCOMPARE(merge.parents().size(), 2);
  QCOMPARE(merge.parents().at(1).summary(), QString("S"));

  Commit o = merge.parents().first().parents().first();
  QCOMPARE(o.id().toString(), mOther);
}

void TestRebaseMerges::cancelKeepsBranch() {
  if (Command::gitPath().isEmpty())
    QSKIP("git is not installed");
  if (GitEditor::helperPath().isEmpty())
    QSKIP("gittyup-editor is not built");

  createMergeHistory();
  Id before = mRepo.head().target().id();
  RepoView *view = openView();
  QVERIFY(view);

  view->interactiveRebaseOnto(mRepo.lookupRef("refs/heads/other"));
  RebaseTodoDialog *todo = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((todo = visibleDialog<RebaseTodoDialog>()), 30000);
  todo->reject();

  QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 60000);
  QVERIFY(!RebaseState::read(mRepo).isValid());
  QCOMPARE(mRepo.head().target().id(), before);
}

void TestRebaseMerges::conflictStopsAndAborts() {
  if (Command::gitPath().isEmpty())
    QSKIP("git is not installed");
  if (GitEditor::helperPath().isEmpty())
    QSKIP("gittyup-editor is not built");

  createMergeHistory(true);
  Id before = mRepo.head().target().id();
  RepoView *view = openView();
  QVERIFY(view);

  view->interactiveRebaseOnto(mRepo.lookupRef("refs/heads/other"));
  RebaseTodoDialog *todo = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((todo = visibleDialog<RebaseTodoDialog>()), 30000);
  todo->accept();

  // A conflicts with O; git stops and the banner takes over.
  QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 60000);
  RebaseState state = RebaseState::read(mRepo);
  QVERIFY(state.isValid());
  QVERIFY(state.owner() == RebaseState::Owner::External);

  view->abortRebase();
  QVERIFY(view->isRunningGit());
  QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 60000);
  QVERIFY(!RebaseState::read(mRepo).isValid());
  QCOMPARE(mRepo.head().target().id(), before);
}

// From a commit below the merge: git keeps the commits on their base.
void TestRebaseMerges::rebaseFromCommit() {
  if (Command::gitPath().isEmpty())
    QSKIP("git is not installed");
  if (GitEditor::helperPath().isEmpty())
    QSKIP("gittyup-editor is not built");

  createMergeHistory();
  Commit b = mRepo.head().target();
  Commit merge = b.parents().first();
  Commit a = merge.parents().first();
  Commit base = a.parents().first();
  RepoView *view = openView();
  QVERIFY(view);

  // Only B: no merge, the dialog of Gittyup.
  view->interactiveRebaseFrom(b);
  QVERIFY(!view->isRunningGit());
  InteractiveRebaseDialog *own = nullptr;
  QTRY_VERIFY((own = visibleDialog<InteractiveRebaseDialog>()));
  own->reject();

  view->interactiveRebaseFrom(a);
  QVERIFY(view->isRunningGit());

  RebaseTodoDialog *todo = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((todo = visibleDialog<RebaseTodoDialog>()), 30000);
  int row = rowOf(todo, "A");
  QVERIFY(row >= 0);
  QVERIFY(todo->setCommand(row, "reword"));
  todo->accept();

  GitMessageDialog *message = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((message = visibleDialog<GitMessageDialog>()),
                           30000);
  message->setMessage("A reworded");
  message->accept();

  QTRY_VERIFY_WITH_TIMEOUT(!view->isRunningGit(), 60000);
  QVERIFY(!RebaseState::read(mRepo).isValid());

  QCOMPARE(firstParentSummaries(4),
           QStringList({"B", "Merge side", "A reworded", "base"}));
  Commit newMerge = mRepo.head().target().parents().first();
  QCOMPARE(newMerge.parents().size(), 2);
  QCOMPARE(newMerge.parents().at(1).summary(), QString("S"));
  QCOMPARE(newMerge.parents().first().parents().first().id(), base.id());
}

TEST_MAIN(TestRebaseMerges)

#include "rebaseMerges.moc"
