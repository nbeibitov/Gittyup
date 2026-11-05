//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Test.h"
#include "editor/TextEditor.h"
#include "git/Command.h"
#include "git/ConflictMarkers.h"
#include "git/Index.h"
#include "ui/ConflictDialog.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

using namespace Test;
using namespace QTest;
using Markers = git::ConflictMarkers;

class TestConflictDialog : public QObject {
  Q_OBJECT

private slots:
  void parse();
  void parseWithBase();
  void parseIgnoresPlainText();
  void parseKeepsLineEndings();
  void version();
  void resolveBlocks();
  void resolveBoth();
  void editedResult();
  void partialSaveKeepsMarkers();
  void movesToNextFile();
  void closeAsksAboutChanges();
  void unknownEncodingCantBeSaved();
  void scrollingStaysAligned();
  void opensFromTheConflictBar();

private:
  struct Scratch {
    ScratchRepository repo;
    QDir workdir{repo->workdir()};

    bool run(const QStringList &args) {
      QProcess process;
      process.setWorkingDirectory(workdir.path());
      process.start(git::Command::gitPath(), args);
      return process.waitForFinished(60000) &&
             process.exitStatus() == QProcess::NormalExit;
    }

    QByteArray read(const QString &name) {
      QFile file(workdir.filePath(name));
      return file.open(QFile::ReadOnly) ? file.readAll() : QByteArray();
    }

    bool write(const QString &name, const QByteArray &text) {
      QFile file(workdir.filePath(name));
      return file.open(QFile::WriteOnly) && file.write(text) == text.size();
    }

    bool commit(const QMap<QString, QByteArray> &files) {
      for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (!write(it.key(), it.value()))
          return false;
      }
      return run({"add", "."}) && run({"commit", "-q", "-m", "change"});
    }

    // Leaves the conflicts of a merge: ours and theirs replace base.
    bool conflict(const QMap<QString, QByteArray> &base,
                  const QMap<QString, QByteArray> &ours,
                  const QMap<QString, QByteArray> &theirs) {
      if (!run({"config", "user.name", "Test"}) ||
          !run({"config", "user.email", "test@example.com"}) ||
          !run({"config", "core.autocrlf", "false"}) || !commit(base) ||
          !run({"checkout", "-q", "-b", "other"}) || !commit(theirs) ||
          !run({"checkout", "-q", "-"}) || !commit(ours))
        return false;

      run({"merge", "-q", "other"}); // fails with the conflicts
      return repo->index().hasConflicts();
    }
  };

  // Two blocks in the same file.
  static QByteArray twoBlocks(const QByteArray &first,
                              const QByteArray &second) {
    QByteArray text = first + "\n";
    for (int i = 0; i < 12; ++i)
      text += "line " + QByteArray::number(i) + "\n";
    return text + second + "\n";
  }

  static TextEditor *pane(ConflictDialog &dialog, const char *name) {
    return dialog.findChild<TextEditor *>(name);
  }

  static QByteArray text(TextEditor *editor) {
    return editor->textRange(0, editor->textLength());
  }

  static void click(ConflictDialog &dialog, const char *name) {
    auto button = dialog.findChild<QAbstractButton *>(name);
    QVERIFY(button);
    QVERIFY(button->isEnabled());
    button->click();
  }

  static QString status(ConflictDialog &dialog) {
    return dialog.findChild<QLabel *>("ConflictStatus")->text();
  }
};

void TestConflictDialog::parse() {
  QByteArray text = "a\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> other\nb\n";
  QList<Markers::Block> blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 1);

  const Markers::Block &block = blocks.first();
  QCOMPARE(block.begin, 2);
  QCOMPARE(block.end, text.size() - 2);
  QCOMPARE(block.oursLabel, QByteArray("HEAD"));
  QCOMPARE(block.theirsLabel, QByteArray("other"));
  QCOMPARE(Markers::resolution(text, block, Markers::Ours),
           QByteArray("ours\n"));
  QCOMPARE(Markers::resolution(text, block, Markers::Theirs),
           QByteArray("theirs\n"));
  QCOMPARE(Markers::resolution(text, block, Markers::Ours | Markers::Theirs),
           QByteArray("ours\ntheirs\n"));
  QCOMPARE(Markers::resolution(text, block, 0), QByteArray());
}

void TestConflictDialog::parseWithBase() {
  QByteArray text = "<<<<<<< HEAD\nours\n||||||| base\nold\n=======\ntheirs\n"
                    ">>>>>>> other\n";
  QList<Markers::Block> blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 1);
  QCOMPARE(Markers::resolution(text, blocks.first(), Markers::Ours),
           QByteArray("ours\n"));
  QCOMPARE(Markers::resolution(text, blocks.first(), Markers::Theirs),
           QByteArray("theirs\n"));
}

void TestConflictDialog::parseIgnoresPlainText() {
  QVERIFY(Markers::parse("").isEmpty());
  QVERIFY(Markers::parse("=======\n>>>>>>> a\n").isEmpty());

  // Never closed.
  QVERIFY(Markers::parse("<<<<<<< a\nours\n=======\ntheirs\n").isEmpty());

  // A run of equal signs is no marker, and the block goes on after it.
  QByteArray text = "<<<<<<< a\n==========\nours\n=======\ntheirs\n>>>>>>> b\n";
  QList<Markers::Block> blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 1);
  QCOMPARE(Markers::resolution(text, blocks.first(), Markers::Ours),
           QByteArray("==========\nours\n"));

  // Only the block that was closed counts.
  text = "<<<<<<< a\nlost\n<<<<<<< b\nours\n=======\ntheirs\n>>>>>>> c\n";
  blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 1);
  QCOMPARE(blocks.first().oursLabel, QByteArray("b"));
}

void TestConflictDialog::parseKeepsLineEndings() {
  QByteArray text =
      "a\r\n<<<<<<< HEAD\r\nours\r\n=======\r\ntheirs\r\n>>>>>>> other\r\nb\r\n";
  QList<Markers::Block> blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 1);
  QCOMPARE(blocks.first().oursLabel, QByteArray("HEAD"));
  QCOMPARE(Markers::resolution(text, blocks.first(), Markers::Ours),
           QByteArray("ours\r\n"));
}

void TestConflictDialog::version() {
  QByteArray text = "a\n<<<<<<< x\n1\n=======\n2\n3\n>>>>>>> y\nb\n"
                    "<<<<<<< x\n=======\n4\n>>>>>>> y\nc\n";
  QList<Markers::Block> blocks = Markers::parse(text);
  QCOMPARE(blocks.size(), 2);

  Markers::Version ours = Markers::version(text, blocks, Markers::Ours);
  QCOMPARE(ours.text, QByteArray("a\n1\nb\nc\n"));
  QCOMPARE(ours.blocks.at(0).begin, 2);
  QCOMPARE(ours.blocks.at(0).end, 4);
  QCOMPARE(ours.blocks.at(1).begin, ours.blocks.at(1).end); // no lines

  Markers::Version theirs = Markers::version(text, blocks, Markers::Theirs);
  QCOMPARE(theirs.text, QByteArray("a\n2\n3\nb\n4\nc\n"));
}

void TestConflictDialog::resolveBlocks() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", twoBlocks("base", "base")}},
                           {{"a.txt", twoBlocks("ours", "ours 2")}},
                           {{"a.txt", twoBlocks("theirs", "theirs 2")}}));

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  QCOMPARE(text(pane(dialog, "ConflictOursPane")),
           twoBlocks("ours", "ours 2"));
  QCOMPARE(text(pane(dialog, "ConflictTheirsPane")),
           twoBlocks("theirs", "theirs 2"));
  QCOMPARE(text(pane(dialog, "ConflictResultPane")), scratch.read("a.txt"));
  QCOMPARE(dialog.findChild<QLabel *>("ConflictOursLabel")->text(),
           QString("Ours: HEAD"));
  QCOMPARE(dialog.findChild<QLabel *>("ConflictTheirsLabel")->text(),
           QString("Theirs: other"));
  QCOMPARE(status(dialog), QString("Conflict 1 of 2"));

  // Navigation wraps around.
  click(dialog, "ConflictNext");
  QCOMPARE(status(dialog), QString("Conflict 2 of 2"));
  click(dialog, "ConflictNext");
  QCOMPARE(status(dialog), QString("Conflict 1 of 2"));
  click(dialog, "ConflictPrevious");
  QCOMPARE(status(dialog), QString("Conflict 2 of 2"));
  click(dialog, "ConflictNext");

  // Every choice goes on to the next block, and can be undone.
  click(dialog, "ConflictOurs");
  QCOMPARE(status(dialog), QString("Conflict 1 of 1"));
  click(dialog, "ConflictUndo");
  QCOMPARE(status(dialog), QString("Conflict 1 of 2"));
  click(dialog, "ConflictOurs");
  click(dialog, "ConflictTheirs");
  QCOMPARE(status(dialog), QString("No conflicts left in the file"));
  QVERIFY(!dialog.findChild<QPushButton *>("ConflictOurs")->isEnabled());

  QByteArray expected = twoBlocks("ours", "theirs 2");
  QCOMPARE(text(pane(dialog, "ConflictResultPane")), expected);

  // The last file closes the window, solved and staged.
  QVERIFY(scratch.read("a.txt") != expected);
  click(dialog, "ConflictSave");
  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QVERIFY(!dialog.isVisible());
  QCOMPARE(scratch.read("a.txt"), expected);
  QVERIFY(!scratch.repo->index().hasConflicts());
}

void TestConflictDialog::resolveBoth() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QByteArray ours = "start\r\nours\r\nend\r\n";
  QByteArray theirs = "start\r\ntheirs\r\nend\r\n";
  QVERIFY(scratch.conflict({{"a.txt", "start\r\nbase\r\nend\r\n"}},
                           {{"a.txt", ours}}, {{"a.txt", theirs}}));

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  click(dialog, "ConflictBoth");
  click(dialog, "ConflictSave");

  // Line endings stay.
  QCOMPARE(scratch.read("a.txt"),
           QByteArray("start\r\nours\r\ntheirs\r\nend\r\n"));
}

void TestConflictDialog::editedResult() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", twoBlocks("base", "base")}},
                           {{"a.txt", twoBlocks("ours", "ours 2")}},
                           {{"a.txt", twoBlocks("theirs", "theirs 2")}}));

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  // Text typed before a block moves the block, and the buttons follow it.
  TextEditor *result = pane(dialog, "ConflictResultPane");
  result->insertText(0, "typed\n");
  result->gotoPos(result->positionFromLine(2));
  QTRY_COMPARE(status(dialog), QString("Conflict 1 of 2"));

  click(dialog, "ConflictTheirs");
  click(dialog, "ConflictTheirs");
  QCOMPARE(text(result), "typed\n" + twoBlocks("theirs", "theirs 2"));

  // Removing the markers by hand solves a block too.
  click(dialog, "ConflictUndo");
  QTRY_COMPARE(status(dialog), QString("Conflict 1 of 1"));
  QByteArray hand = "typed\n" + twoBlocks("theirs", "by hand");
  result->setTargetRange(0, result->textLength());
  result->replaceTarget(hand.size(), hand.constData());
  QTRY_COMPARE(status(dialog), QString("No conflicts left in the file"));

  click(dialog, "ConflictSave");
  QCOMPARE(scratch.read("a.txt"), hand);
  QVERIFY(!scratch.repo->index().hasConflicts());
}

void TestConflictDialog::partialSaveKeepsMarkers() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", twoBlocks("base", "base")}},
                           {{"a.txt", twoBlocks("ours", "ours 2")}},
                           {{"a.txt", twoBlocks("theirs", "theirs 2")}}));

  {
    ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
    dialog.show();
    QVERIFY(qWaitForWindowExposed(&dialog));

    click(dialog, "ConflictOurs");
    click(dialog, "ConflictSave");
    QVERIFY(dialog.isVisible()); // One block is left.
    QVERIFY(scratch.repo->index().hasConflicts());

    QList<Markers::Block> blocks = Markers::parse(scratch.read("a.txt"));
    QCOMPARE(blocks.size(), 1);

    // Nothing to ask about.
    click(dialog, "ConflictClose");
    QVERIFY(!dialog.isVisible());
  }

  // Goes on where it was left, and still shows the versions of the block.
  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));
  QCOMPARE(status(dialog), QString("Conflict 1 of 1"));

  click(dialog, "ConflictTheirs");
  click(dialog, "ConflictSave");
  QCOMPARE(scratch.read("a.txt"), twoBlocks("ours", "theirs 2"));
  QVERIFY(!scratch.repo->index().hasConflicts());
}

void TestConflictDialog::movesToNextFile() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", "base\n"}, {"b.txt", "base\n"}},
                           {{"a.txt", "ours a\n"}, {"b.txt", "ours b\n"}},
                           {{"a.txt", "theirs a\n"}, {"b.txt", "theirs b\n"}}));

  // Starts at the requested file.
  ConflictDialog dialog(scratch.repo, {"a.txt", "b.txt"}, "b.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  auto files = dialog.findChild<QComboBox *>("ConflictFile");
  QCOMPARE(files->currentText(), QString("b.txt"));
  QCOMPARE(text(pane(dialog, "ConflictOursPane")), QByteArray("ours b\n"));

  click(dialog, "ConflictOurs");
  click(dialog, "ConflictSave");
  QVERIFY(dialog.isVisible());
  QCOMPARE(files->count(), 1);
  QCOMPARE(files->currentText(), QString("a.txt"));
  QCOMPARE(text(pane(dialog, "ConflictOursPane")), QByteArray("ours a\n"));
  QCOMPARE(status(dialog), QString("Conflict 1 of 1"));

  click(dialog, "ConflictTheirs");
  click(dialog, "ConflictSave");
  QVERIFY(!dialog.isVisible());
  QCOMPARE(scratch.read("a.txt"), QByteArray("theirs a\n"));
  QCOMPARE(scratch.read("b.txt"), QByteArray("ours b\n"));
  QVERIFY(!scratch.repo->index().hasConflicts());
}

void TestConflictDialog::closeAsksAboutChanges() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", "base\n"}}, {{"a.txt", "ours\n"}},
                           {{"a.txt", "theirs\n"}}));
  QByteArray conflicted = scratch.read("a.txt");

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));
  click(dialog, "ConflictOurs");

  auto answer = [](QMessageBox::StandardButton button) {
    QTimer::singleShot(0, [button] {
      auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      QVERIFY(box);
      box->button(button)->click();
    });
  };

  answer(QMessageBox::Cancel);
  dialog.reject();
  QVERIFY(dialog.isVisible());
  QCOMPARE(scratch.read("a.txt"), conflicted);

  answer(QMessageBox::Discard);
  dialog.reject();
  QVERIFY(!dialog.isVisible());
  QCOMPARE(scratch.read("a.txt"), conflicted);
  QVERIFY(scratch.repo->index().hasConflicts());
}

void TestConflictDialog::unknownEncodingCantBeSaved() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", "base\n"}}, {{"a.txt", "ours\n"}},
                           {{"a.txt", "theirs\n"}}));

  // Neither UTF-8 nor, as this overlong form, anything a code page gives back.
  QByteArray bytes = scratch.read("a.txt") + QByteArray("\x00\xC0\x80\n", 4);
  QVERIFY(scratch.write("a.txt", bytes));

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  QCOMPARE(status(dialog),
           QString("Can't be saved: unknown encoding of the file"));
  QVERIFY(!dialog.findChild<QPushButton *>("ConflictSave")->isEnabled());
  QVERIFY(!dialog.findChild<QPushButton *>("ConflictOurs")->isEnabled());
  QCOMPARE(scratch.read("a.txt"), bytes);
}

void TestConflictDialog::scrollingStaysAligned() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  // Ours has far more lines in the block, so lines drift apart after it.
  auto file = [](const QByteArray &block) {
    QByteArray text;
    for (int i = 0; i < 100; ++i)
      text += "before " + QByteArray::number(i) + "\n";
    text += block;
    for (int i = 0; i < 500; ++i)
      text += "after " + QByteArray::number(i) + "\n";
    return text;
  };

  QByteArray big;
  for (int i = 0; i < 50; ++i)
    big += "ours " + QByteArray::number(i) + "\n";

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", file("base\n")}}, {{"a.txt", file(big)}},
                           {{"a.txt", file("theirs\n")}}));

  ConflictDialog dialog(scratch.repo, {"a.txt"}, "a.txt");
  dialog.resize(1000, 400);
  dialog.show();
  QVERIFY(qWaitForWindowExposed(&dialog));

  TextEditor *ours = pane(dialog, "ConflictOursPane");
  TextEditor *result = pane(dialog, "ConflictResultPane");
  TextEditor *theirs = pane(dialog, "ConflictTheirsPane");

  // A block puts all of the panes at its first line.
  QCOMPARE(ours->firstVisibleLine(), 97);
  QCOMPARE(theirs->firstVisibleLine(), 97);
  QCOMPARE(result->firstVisibleLine(), 97);

  // Past the block they show the same text, though at other line numbers.
  auto firstLine = [](TextEditor *editor) {
    int line = editor->firstVisibleLine();
    return editor->textRange(editor->positionFromLine(line),
                             editor->lineEndPosition(line));
  };

  ours->setFirstVisibleLine(300);
  QCOMPARE(firstLine(ours), QByteArray("after 150"));
  QCOMPARE(firstLine(theirs), QByteArray("after 150"));
  QCOMPARE(firstLine(result), QByteArray("after 150"));

  theirs->setFirstVisibleLine(20);
  QCOMPARE(firstLine(ours), QByteArray("before 20"));
  QCOMPARE(firstLine(result), QByteArray("before 20"));
}

void TestConflictDialog::opensFromTheConflictBar() {
  if (git::Command::gitPath().isEmpty())
    QSKIP("git is not installed");

  Scratch scratch;
  QVERIFY(scratch.conflict({{"a.txt", "base\n"}}, {{"a.txt", "ours\n"}},
                           {{"a.txt", "theirs\n"}}));

  MainWindow window(scratch.repo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));

  RepoView *view = window.currentView();
  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto bar = doubleTree->findChild<QWidget *>("ConflictBar");
  auto open = doubleTree->findChild<QToolButton *>("ConflictResolve");
  QVERIFY(bar && open);
  QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), 30000);
  QVERIFY(open->isEnabled());

  open->click();
  auto dialog = doubleTree->findChild<ConflictDialog *>();
  QVERIFY(dialog);
  QTRY_VERIFY(dialog->isVisible());

  click(*dialog, "ConflictTheirs");
  click(*dialog, "ConflictSave");

  // Nothing is left to solve, and the window is gone.
  QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 30000);
  QTRY_VERIFY(!doubleTree->findChild<ConflictDialog *>());
  QCOMPARE(scratch.read("a.txt"), QByteArray("theirs\n"));
}

TEST_MAIN(TestConflictDialog)
#include "conflictDialog.moc"
