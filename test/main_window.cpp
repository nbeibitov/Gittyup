//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "git/Config.h"
#include "ui/MainWindow.h"

using namespace Test;
using namespace QTest;

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void show();
  void invalidRepoReason();
  void cleanupTestCase();

private:
  ScratchRepository mRepo;
  MainWindow *mWindow = nullptr;
};

void TestMainWindow::initTestCase() { mWindow = new MainWindow(mRepo); }

void TestMainWindow::show() {
  mWindow->show();
  QVERIFY(qWaitForWindowActive(mWindow));
}

// libgit2 refuses repositories with extensions it doesn't support.
void TestMainWindow::invalidRepoReason() {
  struct Case {
    QString extension;
    QString value;
    QString reason;
  };
  QList<Case> cases = {{"partialClone", "origin", "partial clone"},
                       {"refStorage", "reftable", "reftable"},
                       {"someFutureThing", "true", "someFutureThing"}};

  for (const Case &c : cases) {
    ScratchRepository scratch;
    QString path = git::Repository(scratch).workdir().path();
    git::Config config = git::Repository(scratch).gitConfig();
    config.setValue("core.repositoryformatversion", 1);
    config.setValue("extensions." + c.extension, c.value);

    QVERIFY(!git::Repository::open(path).isValid());
    QString reason = MainWindow::invalidRepoReason();
    QVERIFY2(reason.contains(c.reason, Qt::CaseInsensitive),
             qPrintable(reason));
  }
}

void TestMainWindow::cleanupTestCase() { mWindow->close(); }

TEST_MAIN(TestMainWindow)

#include "main_window.moc"
