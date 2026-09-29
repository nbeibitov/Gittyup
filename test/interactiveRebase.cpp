//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "dialogs/InteractiveRebaseDialog.h"
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Index.h"
#include "git/InteractiveRebase.h"
#include "git/Reference.h"
#include "git/Signature.h"
#include "git/Tree.h"
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>

using namespace git;
using Action = InteractiveRebase::Action;
using Status = InteractiveRebase::Status;
using Step = InteractiveRebase::Step;

#define VERIFY_STATUS(result, expected)                                        \
  QVERIFY2((result).status == (expected), qPrintable((result).error))

class TestInteractiveRebase : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void commitsInRange();
  void pickAllKeepsHistory();
  void reorder();
  void drop();
  void reword();
  void squash();
  void squashDefaultMessage();
  void fixup();
  void editStopAndContinue();
  void conflictResolveAndContinue();
  void abortRestoresBranch();
  void skipConflictingCommit();
  void autosquash();
  void dirtyWorkdir();
  void autostash();
  void validate();
  void committerDateIsAuthorDate();

  void dialogSteps();
  void dialogValidation();
  void dialogSquashMessage();
  void dialogAutosquash();
  void dialogIncludeOlderCommit();
  void onto();
  void dialogOnto();

private:
  void writeFile(const QString &name, const QString &content);
  QString readFile(const QString &name) const;
  Commit commitFile(const QString &name, const QString &content,
                    const QString &message);
  void createThreeCommits();
  void createConflictingCommits();
  QStringList history() const;
  QString branchName() const;
  static Step step(Action action, const Commit &commit,
                   const QString &message = QString());

  QScopedPointer<Test::ScratchRepository> mScratch;
  Repository mRepo;
  Commit mBase;
  Commit mA, mB, mC;
  QString mBranch;
};

void TestInteractiveRebase::init() {
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;
  mBase = commitFile("base.txt", "base\n", "Base");
  QVERIFY(mBase.isValid());
  mBranch = branchName();
  QVERIFY(!mBranch.isEmpty());
}

void TestInteractiveRebase::cleanup() {
  mRepo = Repository();
  mScratch.reset();
}

void TestInteractiveRebase::writeFile(const QString &name,
                                      const QString &content) {
  QFile file(mRepo.workdir().filePath(name));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  file.write(content.toUtf8());
}

QString TestInteractiveRebase::readFile(const QString &name) const {
  QFile file(mRepo.workdir().filePath(name));
  if (!file.open(QFile::ReadOnly))
    return QString();

  // Ignore line ending conversion (core.autocrlf).
  return QString::fromUtf8(file.readAll()).remove('\r');
}

Commit TestInteractiveRebase::commitFile(const QString &name,
                                         const QString &content,
                                         const QString &message) {
  writeFile(name, content);
  mRepo.index().setStaged({name}, true, false);
  return mRepo.commit(message);
}

void TestInteractiveRebase::createThreeCommits() {
  mA = commitFile("a.txt", "a\n", "A");
  mB = commitFile("b.txt", "b\n", "B");
  mC = commitFile("c.txt", "c\n", "C");
  QVERIFY(mA.isValid() && mB.isValid() && mC.isValid());
}

// base: f=1, A: f=2 ("X"), B: f=3 ("Y"). Reordering them conflicts.
void TestInteractiveRebase::createConflictingCommits() {
  commitFile("f.txt", "1\n", "F");
  mBase = mRepo.head().target();
  mA = commitFile("f.txt", "2\n", "X");
  mB = commitFile("f.txt", "3\n", "Y");
  QVERIFY(mA.isValid() && mB.isValid());
}

QStringList TestInteractiveRebase::history() const {
  QStringList result;
  Commit commit = mRepo.head().target();
  while (commit.isValid() && commit.id() != mBase.id()) {
    result.prepend(commit.summary());
    QList<Commit> parents = commit.parents();
    commit = parents.isEmpty() ? Commit() : parents.first();
  }
  return result;
}

QString TestInteractiveRebase::branchName() const {
  Reference head = mRepo.head();
  return head.isValid() ? head.qualifiedName() : QString();
}

Step TestInteractiveRebase::step(Action action, const Commit &commit,
                                 const QString &message) {
  Step result;
  result.action = action;
  result.commit = commit.id();
  result.message = message;
  return result;
}

void TestInteractiveRebase::commitsInRange() {
  createThreeCommits();

  QString error;
  QList<Commit> commits = InteractiveRebase::commits(mRepo, mBase, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(commits.size(), 3);
  QCOMPARE(commits.at(0).id(), mA.id());
  QCOMPARE(commits.at(2).id(), mC.id());

  // The root commit has no parent to rebase onto.
  commits = InteractiveRebase::commits(mRepo, Commit(), &error);
  QVERIFY(commits.isEmpty());
  QVERIFY(!error.isEmpty());
}

void TestInteractiveRebase::pickAllKeepsHistory() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto steps = InteractiveRebase::defaultSteps({mA, mB, mC});
  auto result = rebase.start(mBase, steps, {});
  VERIFY_STATUS(result, Status::Finished);

  // Nothing changed, so the original commits are reused.
  QCOMPARE(Commit(mRepo.head().target()).id(), mC.id());
  QCOMPARE(branchName(), mBranch);
  QVERIFY(!mRepo.isHeadDetached());
  QVERIFY(!InteractiveRebase::isInProgress(mRepo));
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_NONE);
}

void TestInteractiveRebase::reorder() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mC), step(Action::Pick, mA), step(Action::Pick, mB)},
      {});
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"C", "A", "B"}));
  QCOMPARE(readFile("a.txt"), "a\n");
  QCOMPARE(readFile("b.txt"), "b\n");
  QCOMPARE(readFile("c.txt"), "c\n");
  QCOMPARE(branchName(), mBranch);
  QVERIFY(!mRepo.isHeadDetached());
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_NONE);

  // Author information is preserved.
  Commit head = mRepo.head().target();
  QCOMPARE(head.author().name(), mB.author().name());
  QCOMPARE(head.author().date(), mB.author().date());
}

void TestInteractiveRebase::drop() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mA), step(Action::Drop, mB), step(Action::Pick, mC)},
      {});
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"A", "C"}));
  QVERIFY(!QFile::exists(mRepo.workdir().filePath("b.txt")));
}

void TestInteractiveRebase::reword() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(mBase,
                             {step(Action::Pick, mA),
                              step(Action::Reword, mB, "B reworded\n\nBody"),
                              step(Action::Pick, mC)},
                             {});
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"A", "B reworded", "C"}));
  Commit reworded = Commit(mRepo.head().target()).parents().first();
  QCOMPARE(reworded.message(), "B reworded\n\nBody\n");
  QVERIFY(reworded.tree() == mB.tree());
}

void TestInteractiveRebase::squash() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result =
      rebase.start(mBase,
                   {step(Action::Pick, mA), step(Action::Squash, mB, "A and B"),
                    step(Action::Pick, mC)},
                   {});
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"A and B", "C"}));
  Commit squashed = Commit(mRepo.head().target()).parents().first();
  QCOMPARE(squashed.author().date(), mA.author().date());
  QCOMPARE(squashed.parents().first().id(), mBase.id());
  QCOMPARE(readFile("a.txt"), "a\n");
  QCOMPARE(readFile("b.txt"), "b\n");
}

void TestInteractiveRebase::squashDefaultMessage() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(mBase,
                             {step(Action::Pick, mA), step(Action::Squash, mB),
                              step(Action::Squash, mC)},
                             {});
  VERIFY_STATUS(result, Status::Finished);

  Commit head = mRepo.head().target();
  QCOMPARE(head.message(), "A\n\nB\n\nC");
  QCOMPARE(head.parents().first().id(), mBase.id());
}

void TestInteractiveRebase::fixup() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mA), step(Action::Fixup, mB), step(Action::Pick, mC)},
      {});
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"A", "C"}));
  QCOMPARE(readFile("b.txt"), "b\n");
}

void TestInteractiveRebase::editStopAndContinue() {
  createThreeCommits();

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mA), step(Action::Edit, mB), step(Action::Pick, mC)},
      {});
  VERIFY_STATUS(result, Status::Edit);
  QCOMPARE(result.step, 1);
  QVERIFY(InteractiveRebase::isInProgress(mRepo));
  QVERIFY(mRepo.rebaseOngoing());
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_REBASE_INTERACTIVE);
  QVERIFY(mRepo.isHeadDetached());
  QCOMPARE(Commit(mRepo.head().target()).id(), mB.id());

  // Uncommitted changes block continuing.
  writeFile("b.txt", "changed\n");
  result = rebase.resume();
  VERIFY_STATUS(result, Status::Error);
  QVERIFY(InteractiveRebase::isInProgress(mRepo));

  // Add a commit while stopped.
  QVERIFY(commitFile("b.txt", "changed\n", "Extra").isValid());

  result = rebase.resume();
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"A", "B", "Extra", "C"}));
  QCOMPARE(readFile("b.txt"), "changed\n");
  QCOMPARE(branchName(), mBranch);
  QVERIFY(!InteractiveRebase::isInProgress(mRepo));
}

void TestInteractiveRebase::conflictResolveAndContinue() {
  createConflictingCommits();

  InteractiveRebase rebase(mRepo);
  auto result =
      rebase.start(mBase, {step(Action::Pick, mB), step(Action::Pick, mA)}, {});
  VERIFY_STATUS(result, Status::Conflict);
  QCOMPARE(result.step, 0);
  QVERIFY(mRepo.index().hasConflicts());
  QVERIFY(rebase.isStoppedAtConflict());
  QCOMPARE(rebase.pendingMessage(), "Y");
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_REBASE_INTERACTIVE);

  // Continuing with unresolved conflicts fails.
  result = rebase.resume();
  VERIFY_STATUS(result, Status::Error);

  // Resolve by staging the file, then continue with a custom message.
  writeFile("f.txt", "3\n");
  mRepo.index().setStaged({"f.txt"}, true, false);
  QVERIFY(!mRepo.index().hasConflicts());
  result = rebase.resume("Y resolved");

  // Applying X on top of Y conflicts again.
  VERIFY_STATUS(result, Status::Conflict);
  QCOMPARE(result.step, 1);
  QCOMPARE(rebase.pendingMessage(), "X");

  writeFile("f.txt", "2\n");
  mRepo.index().setStaged({"f.txt"}, true, false);
  result = rebase.resume();
  VERIFY_STATUS(result, Status::Finished);

  QCOMPARE(history(), QStringList({"Y resolved", "X"}));
  QCOMPARE(readFile("f.txt"), "2\n");
  QCOMPARE(branchName(), mBranch);
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_NONE);
}

void TestInteractiveRebase::abortRestoresBranch() {
  createConflictingCommits();
  Id tip = Commit(mRepo.head().target()).id();

  InteractiveRebase rebase(mRepo);
  auto result =
      rebase.start(mBase, {step(Action::Pick, mB), step(Action::Pick, mA)}, {});
  VERIFY_STATUS(result, Status::Conflict);

  QString error;
  QVERIFY2(rebase.abort(&error), qPrintable(error));
  QVERIFY(!InteractiveRebase::isInProgress(mRepo));
  QCOMPARE(mRepo.state(), GIT_REPOSITORY_STATE_NONE);
  QCOMPARE(branchName(), mBranch);
  QVERIFY(!mRepo.isHeadDetached());
  QCOMPARE(Commit(mRepo.head().target()).id(), tip);
  QCOMPARE(readFile("f.txt"), "3\n");
  QVERIFY(!mRepo.index().hasConflicts());
}

void TestInteractiveRebase::skipConflictingCommit() {
  createConflictingCommits();

  InteractiveRebase rebase(mRepo);
  auto result =
      rebase.start(mBase, {step(Action::Pick, mB), step(Action::Pick, mA)}, {});
  VERIFY_STATUS(result, Status::Conflict);

  result = rebase.skip();
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"X"}));
  QCOMPARE(readFile("f.txt"), "2\n");
}

void TestInteractiveRebase::autosquash() {
  mA = commitFile("a.txt", "a\n", "A");
  mB = commitFile("b.txt", "b\n", "B");
  Commit fixupA = commitFile("a.txt", "a2\n", "fixup! A");
  Commit squashB = commitFile("b.txt", "b2\n", "squash! B");

  auto steps = InteractiveRebase::autosquash(
      mRepo, InteractiveRebase::defaultSteps({mA, mB, fixupA, squashB}));
  QCOMPARE(steps.size(), 4);
  QCOMPARE(steps.at(0).commit, mA.id());
  QCOMPARE(steps.at(1).commit, fixupA.id());
  QVERIFY(steps.at(1).action == Action::Fixup);
  QCOMPARE(steps.at(2).commit, mB.id());
  QCOMPARE(steps.at(3).commit, squashB.id());
  QVERIFY(steps.at(3).action == Action::Squash);

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(mBase, steps, {});
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"A", "B"}));
  QCOMPARE(readFile("a.txt"), "a2\n");
  QCOMPARE(readFile("b.txt"), "b2\n");
}

void TestInteractiveRebase::dirtyWorkdir() {
  createThreeCommits();
  writeFile("a.txt", "dirty\n");

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mC), step(Action::Pick, mA), step(Action::Pick, mB)},
      {});
  VERIFY_STATUS(result, Status::Error);
  QVERIFY(!InteractiveRebase::isInProgress(mRepo));
  QCOMPARE(Commit(mRepo.head().target()).id(), mC.id());
  QCOMPARE(readFile("a.txt"), "dirty\n");
}

void TestInteractiveRebase::autostash() {
  createThreeCommits();
  writeFile("a.txt", "dirty\n");

  InteractiveRebase::Options options;
  options.autostash = true;

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase,
      {step(Action::Pick, mC), step(Action::Pick, mA), step(Action::Pick, mB)},
      options);
  VERIFY_STATUS(result, Status::Finished);
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QCOMPARE(history(), QStringList({"C", "A", "B"}));
  QCOMPARE(readFile("a.txt"), "dirty\n");
  QVERIFY(mRepo.stashes().isEmpty());
}

void TestInteractiveRebase::validate() {
  createThreeCommits();

  QVERIFY(!InteractiveRebase::validate({}).isEmpty());
  QVERIFY(!InteractiveRebase::validate(
               {step(Action::Squash, mA), step(Action::Pick, mB)})
               .isEmpty());
  QVERIFY(!InteractiveRebase::validate(
               {step(Action::Drop, mA), step(Action::Fixup, mB)})
               .isEmpty());
  QVERIFY(!InteractiveRebase::validate(
               {step(Action::Pick, mA), step(Action::Pick, mA)})
               .isEmpty());
  QVERIFY(InteractiveRebase::validate({step(Action::Drop, mA),
                                       step(Action::Pick, mB),
                                       step(Action::Fixup, mC)})
              .isEmpty());

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase, {step(Action::Squash, mA), step(Action::Pick, mB)}, {});
  VERIFY_STATUS(result, Status::Error);
  QVERIFY(!InteractiveRebase::isInProgress(mRepo));
}

void TestInteractiveRebase::committerDateIsAuthorDate() {
  createThreeCommits();

  InteractiveRebase::Options options;
  options.committerDateIsAuthorDate = true;

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(
      mBase, {step(Action::Pick, mB), step(Action::Pick, mA)}, options);
  VERIFY_STATUS(result, Status::Finished);

  Commit head = mRepo.head().target();
  QCOMPARE(head.summary(), "A");
  QCOMPARE(head.committer().date(), mA.author().date());
}

static QPushButton *startButton(QDialog *dialog) {
  for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
    if (button->text() == InteractiveRebaseDialog::tr("Start Rebase"))
      return button;
  }
  return nullptr;
}

void TestInteractiveRebase::dialogSteps() {
  createThreeCommits();

  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mBase);
  QVERIFY(dialog->error().isEmpty());
  QPushButton *start = startButton(dialog);
  QVERIFY(start);

  // Nothing changed yet.
  QCOMPARE(dialog->steps().size(), 3);
  QVERIFY(!start->isEnabled());

  dialog->setAction(1, Action::Drop);
  dialog->moveRow(2, -2);
  QVERIFY(start->isEnabled());

  QList<Step> steps = dialog->steps();
  QCOMPARE(steps.size(), 3);
  QCOMPARE(steps.at(0).commit, mC.id());
  QCOMPARE(steps.at(1).commit, mA.id());
  QCOMPARE(steps.at(2).commit, mB.id());
  QVERIFY(steps.at(2).action == Action::Drop);

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(dialog->base(), steps, dialog->options());
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"C", "A"}));

  delete dialog;
}

void TestInteractiveRebase::dialogValidation() {
  createThreeCommits();

  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mBase);
  QPushButton *start = startButton(dialog);
  QVERIFY(start);

  dialog->setAction(0, Action::Squash);
  QVERIFY(!start->isEnabled());

  dialog->setAction(0, Action::Reword);
  QVERIFY(start->isEnabled());

  delete dialog;

  // The root commit cannot be rebased interactively.
  QVERIFY(!InteractiveRebaseDialog(mRepo, Commit()).error().isEmpty());
}

void TestInteractiveRebase::dialogSquashMessage() {
  createThreeCommits();

  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mBase);
  auto *list = dialog->findChild<QTreeWidget *>("InteractiveRebaseList");
  auto *editor =
      dialog->findChild<QPlainTextEdit *>("InteractiveRebaseMessage");
  QVERIFY(list && editor);

  dialog->setAction(1, Action::Squash);
  list->setCurrentItem(list->topLevelItem(1));
  QVERIFY(!editor->isReadOnly());
  QCOMPARE(editor->toPlainText(), "A\n\nB");

  // The default message is not stored in the plan.
  QVERIFY(dialog->steps().at(1).message.isEmpty());

  editor->setPlainText("A and B");
  QCOMPARE(dialog->steps().at(1).message, "A and B");

  // Pick rows show the message read-only.
  list->setCurrentItem(list->topLevelItem(0));
  QVERIFY(editor->isReadOnly());

  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(dialog->base(), dialog->steps(), {});
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"A and B", "C"}));

  delete dialog;
}

void TestInteractiveRebase::dialogAutosquash() {
  mA = commitFile("a.txt", "a\n", "A");
  mB = commitFile("b.txt", "b\n", "B");
  Commit fixupA = commitFile("a.txt", "a2\n", "fixup! A");

  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mBase);
  dialog->applyAutosquash();

  QList<Step> steps = dialog->steps();
  QCOMPARE(steps.size(), 3);
  QCOMPARE(steps.at(1).commit, fixupA.id());
  QVERIFY(steps.at(1).action == Action::Fixup);
  QCOMPARE(steps.at(2).commit, mB.id());

  delete dialog;
}

void TestInteractiveRebase::dialogIncludeOlderCommit() {
  createThreeCommits();

  // Start with the last commit only, then extend the range backwards.
  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mB);
  QVERIFY(dialog->error().isEmpty());
  QCOMPARE(dialog->steps().size(), 1);

  dialog->setAction(0, Action::Reword);
  dialog->includeOlderCommit();
  QCOMPARE(dialog->base().id(), mA.id());
  QList<Step> steps = dialog->steps();
  QCOMPARE(steps.size(), 2);
  QCOMPARE(steps.at(0).commit, mB.id());
  QVERIFY(steps.at(0).action == Action::Pick);
  QCOMPARE(steps.at(1).commit, mC.id());
  QVERIFY(steps.at(1).action == Action::Reword);

  dialog->includeOlderCommit();
  QCOMPARE(dialog->base().id(), mBase.id());
  QCOMPARE(dialog->steps().size(), 3);

  // The root commit cannot be included.
  if (mBase.parents().isEmpty()) {
    dialog->includeOlderCommit();
    QCOMPARE(dialog->base().id(), mBase.id());
    QCOMPARE(dialog->steps().size(), 3);
  }

  dialog->setAction(0, Action::Drop);
  dialog->setAction(2, Action::Pick);
  InteractiveRebase rebase(mRepo);
  auto result =
      rebase.start(dialog->base(), dialog->steps(), dialog->options());
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"B", "C"}));

  delete dialog;
}

void TestInteractiveRebase::onto() {
  createThreeCommits();

  // Move C from B onto A, which leaves B out (git rebase --onto A B).
  InteractiveRebase::Options options;
  options.onto = mA.id();
  InteractiveRebase rebase(mRepo);
  auto result = rebase.start(mB, {step(Action::Pick, mC)}, options);
  VERIFY_STATUS(result, Status::Finished);
  QCOMPARE(history(), QStringList({"A", "C"}));
  QCOMPARE(branchName(), mBranch);
  QCOMPARE(readFile("b.txt"), QString());
  QCOMPARE(readFile("c.txt"), QString("c\n"));
}

void TestInteractiveRebase::dialogOnto() {
  createThreeCommits();
  git::Branch other = mRepo.createBranch("other", mA);
  QVERIFY(other.isValid());

  // Selecting a branch extends the range back to the merge base.
  InteractiveRebaseDialog *dialog = new InteractiveRebaseDialog(mRepo, mB);
  QVERIFY(dialog->error().isEmpty());
  QCOMPARE(dialog->steps().size(), 1);

  dialog->setOnto(other);
  QCOMPARE(dialog->base().id(), mA.id());
  QCOMPARE(dialog->steps().size(), 2);
  QCOMPARE(dialog->options().onto, mA.id());

  // Back to rewriting in place.
  dialog->setOnto(git::Reference());
  QVERIFY(!dialog->options().onto.isValid());
  QCOMPARE(dialog->steps().size(), 2);

  delete dialog;
}

TEST_MAIN(TestInteractiveRebase)

#include "interactiveRebase.moc"
