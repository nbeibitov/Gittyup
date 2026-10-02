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
#include "git/CommitGraph.h"
#include "git/Config.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "git/RevWalk.h"
#include "git2/commit.h"
#include "git2/refs.h"
#include "git2/revwalk.h"
#include "git2/signature.h"
#include "git2/sys/commit_graph.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <vector>

using namespace git;

class TestCommitGraph : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void writeAndVerify();
  void replaceWhileMapped();
  void outdated();
  void locked();
  void mergesHaveValidGenerations();

private:
  Commit commitFile(const QString &name, const QString &content);
  QStringList walk() const;
  void verifyWithGit();
  QStringList staleFiles() const;

  QScopedPointer<Test::ScratchRepository> mScratch;
  Repository mRepo;
};

void TestCommitGraph::init() {
  mScratch.reset(new Test::ScratchRepository);
  mRepo = *mScratch;
  for (int i = 0; i < 5; ++i)
    QVERIFY(commitFile("file.txt", QString::number(i)).isValid());
}

void TestCommitGraph::cleanup() {
  mRepo = Repository();
  mScratch.reset();
}

Commit TestCommitGraph::commitFile(const QString &name,
                                   const QString &content) {
  QFile file(mRepo.workdir().filePath(name));
  if (!file.open(QFile::WriteOnly | QFile::Truncate))
    return Commit();
  file.write(content.toUtf8());
  file.close();

  mRepo.index().setStaged({name}, true);
  return mRepo.commit(content);
}

// Summaries of the history of HEAD, newest first.
QStringList TestCommitGraph::walk() const {
  QStringList result;
  RevWalk walker = mRepo.head().target().walker(GIT_SORT_TOPOLOGICAL);
  for (Commit commit = walker.next(); commit.isValid(); commit = walker.next())
    result.append(commit.summary());
  return result;
}

void TestCommitGraph::verifyWithGit() {
  QString git = QStandardPaths::findExecutable("git");
  if (git.isEmpty())
    return; // optional check

  QProcess process;
  process.setWorkingDirectory(mRepo.workdir().path());
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(git, {"commit-graph", "verify"});
  QVERIFY(process.waitForFinished(60000));
  QVERIFY2(process.exitCode() == 0, process.readAll().constData());
}

QStringList TestCommitGraph::staleFiles() const {
  QDir dir = QFileInfo(CommitGraph::path(mRepo)).dir();
  return dir.entryList({"commit-graph-*.stale"}, QDir::Files);
}

void TestCommitGraph::writeAndVerify() {
  QString path = CommitGraph::path(mRepo);
  QVERIFY(!path.isEmpty());
  QVERIFY(!QFile::exists(path));

  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  QVERIFY(QFile::exists(path));
  QVERIFY(!QFile::exists(path + ".lock"));
  verifyWithGit();

  // libgit2 walks the history with the commit-graph.
  CommitGraph::reload(mRepo);
  QCOMPARE(walk(), QStringList({"4", "3", "2", "1", "0"}));
}

void TestCommitGraph::replaceWhileMapped() {
  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  CommitGraph::reload(mRepo);

  // Walking maps the commit-graph into memory.
  QCOMPARE(walk().size(), 5);

  QVERIFY(commitFile("file.txt", "5").isValid());
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  verifyWithGit();

  // The replaced file is removed once it is no longer used.
  CommitGraph::reload(mRepo);
  QCOMPARE(walk(), QStringList({"5", "4", "3", "2", "1", "0"}));
  QVERIFY2(staleFiles().isEmpty(), qPrintable(staleFiles().join(", ")));
}

void TestCommitGraph::outdated() {
  // Missing.
  QVERIFY(CommitGraph::isOutdated(mRepo));

  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  QVERIFY(!CommitGraph::isOutdated(mRepo));

  // A new commit is not reason enough to rewrite a recent file.
  QTest::qWait(1100); // file times have a resolution of seconds
  QVERIFY(commitFile("file.txt", "5").isValid());
  QVERIFY(!CommitGraph::isOutdated(mRepo));
  QVERIFY(CommitGraph::isOutdated(mRepo, 0));

  // Disabled.
  mRepo.gitConfig().setValue("core.commitGraph", false);
  QVERIFY(!CommitGraph::isOutdated(mRepo, 0));
}

void TestCommitGraph::locked() {
  QString path = CommitGraph::path(mRepo);
  QVERIFY(QDir().mkpath(QFileInfo(path).path()));

  QFile lock(path + ".lock");
  QVERIFY(lock.open(QFile::WriteOnly));
  lock.close();

  QString error;
  QVERIFY(!CommitGraph::write(mRepo.dir().path(), &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(QFile::exists(path + ".lock")); // someone else's lock is kept
  QVERIFY(!QFile::exists(path));

  QVERIFY(QFile::remove(path + ".lock"));
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
}

// libgit2 computes wrong generation numbers for histories with merges.
void TestCommitGraph::mergesHaveValidGenerations() {
  git_repository *repo = nullptr;
  QVERIFY(!git_repository_open(&repo, mRepo.dir().path().toUtf8()));

  // Merges of branches with and without new commits on the main branch,
  // created directly (running git for each one takes too long).
  git_oid head;
  QVERIFY(!git_reference_name_to_id(&head, repo, "HEAD"));
  git_commit *tip = nullptr;
  QVERIFY(!git_commit_lookup(&tip, repo, &head));
  git_tree *tree = nullptr;
  QVERIFY(!git_commit_tree(&tree, tip));
  git_signature *sig = nullptr;
  QVERIFY(!git_signature_now(&sig, "Test", "test@example.com"));

  auto commit = [&](const char *message, const char *ref,
                    std::vector<const git_commit *> parents) {
    git_oid id;
    git_commit *result = nullptr;
    if (!git_commit_create(&id, repo, ref, sig, sig, nullptr, message, tree,
                           parents.size(), parents.data()))
      git_commit_lookup(&result, repo, &id);
    return result;
  };

  for (int i = 0; i < 40; ++i) {
    git_commit *branch = commit("branch", nullptr, {tip});
    QVERIFY(branch);
    if (i % 2) {
      git_commit *main = commit("main", "HEAD", {tip});
      QVERIFY(main);
      git_commit_free(tip);
      tip = main;
    }

    git_commit *merge = commit("merge", "HEAD", {tip, branch});
    QVERIFY(merge);
    git_commit_free(branch);
    git_commit_free(tip);
    tip = merge;
  }

  git_commit_free(tip);
  git_tree_free(tree);
  git_signature_free(sig);

  // The libgit2 writer alone gets them wrong.
  QString info = QFileInfo(CommitGraph::path(mRepo)).path();
  git_commit_graph_writer *writer = nullptr;
  git_commit_graph_writer_options opts = GIT_COMMIT_GRAPH_WRITER_OPTIONS_INIT;
  QVERIFY(!git_commit_graph_writer_new(&writer, info.toUtf8(), &opts));
  git_revwalk *walk = nullptr;
  QVERIFY(!git_revwalk_new(&walk, repo));
  QVERIFY(!git_revwalk_push_glob(walk, "refs/*"));
  QVERIFY(!git_commit_graph_writer_add_revwalk(writer, walk));
  git_buf buf = GIT_BUF_INIT;
  QVERIFY(!git_commit_graph_writer_dump(&buf, writer));
  QByteArray raw(buf.ptr, static_cast<qsizetype>(buf.size));
  git_buf_dispose(&buf);
  git_revwalk_free(walk);
  git_commit_graph_writer_free(writer);
  git_repository_free(repo);

  QByteArray fixed = raw;
  bool changed = false;
  QVERIFY(CommitGraph::fixGenerations(fixed, &changed));
  QVERIFY2(changed, "the history doesn't reproduce the libgit2 bug");

  // Such a file (written by earlier versions) is detected.
  QString path = CommitGraph::path(mRepo);
  QVERIFY(QDir().mkpath(info));
  {
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write(raw), raw.size());
  }
  QVERIFY(CommitGraph::isCorrupt(path));

  // The written file is valid.
  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  QVERIFY(!CommitGraph::isCorrupt(path));
  verifyWithGit();

  // Fixing a fixed file changes nothing.
  QFile file(path);
  QVERIFY(file.open(QFile::ReadOnly));
  QByteArray data = file.readAll();
  QVERIFY(CommitGraph::fixGenerations(data, &changed));
  QVERIFY(!changed);
}

TEST_MAIN(TestCommitGraph)

#include "commitGraph.moc"
