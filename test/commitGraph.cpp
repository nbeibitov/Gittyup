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
#include "git2/errors.h"
#include "git2/refs.h"
#include "git2/revwalk.h"
#include "git2/signature.h"
#include "git2/sys/commit_graph.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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
  void readsGraphOfGit();
  void writesWithGit();
  void keepsChainOfGit();
  void releasesReplacedFile();

private:
  Commit commitFile(const QString &name, const QString &content);
  QStringList walk() const;
  void verifyWithGit();
  bool runGit(const QStringList &args, QString *output = nullptr) const;
  QByteArray readGraph() const;
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

bool TestCommitGraph::runGit(const QStringList &args, QString *output) const {
  QString git = QStandardPaths::findExecutable("git");
  if (git.isEmpty())
    return false;

  QProcess process;
  process.setWorkingDirectory(mRepo.workdir().path());
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(git, args);
  bool finished = process.waitForFinished(60000);
  if (output)
    *output = QString::fromUtf8(process.readAll());
  return finished && process.exitCode() == 0;
}

QByteArray TestCommitGraph::readGraph() const {
  QFile file(CommitGraph::path(mRepo));
  return file.open(QFile::ReadOnly) ? file.readAll() : QByteArray();
}

namespace {

// True if the commit-graph data has a chunk with the given id, e.g. "GDA2".
bool hasChunk(const QByteArray &data, const char *id) {
  if (data.size() < 8)
    return false;

  int chunks = uchar(data.at(6));
  for (int i = 0; i < chunks; ++i) {
    qint64 entry = 8 + i * 12;
    if (entry + 4 <= data.size() && data.mid(entry, 4) == id)
      return true;
  }

  return false;
}

} // namespace

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

// git writes generation data (GDA2) that the libgit2 of the submodule
// rejected as an unknown chunk, so it ignored every file of newer gits.
void TestCommitGraph::readsGraphOfGit() {
  if (QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("git is not installed");

  QString output;
  QVERIFY2(runGit({"-c", "commitGraph.generationVersion=2", "commit-graph",
                   "write", "--reachable"},
                  &output),
           qPrintable(output));
  QVERIFY(hasChunk(readGraph(), "GDA2"));

  QString objects = QFileInfo(QFileInfo(CommitGraph::path(mRepo)).path())
                        .path();
  git_commit_graph *graph = nullptr;
#ifdef GIT_EXPERIMENTAL_SHA256
  git_commit_graph_open_options opts = GIT_COMMIT_GRAPH_OPEN_OPTIONS_INIT;
  int error = git_commit_graph_open(&graph, objects.toUtf8(), &opts);
#else
  int error = git_commit_graph_open(&graph, objects.toUtf8());
#endif
  const git_error *last = git_error_last();
  QVERIFY2(!error, last && last->message ? last->message : "");
  git_commit_graph_free(graph);

  CommitGraph::reload(mRepo);
  QCOMPARE(walk(), QStringList({"4", "3", "2", "1", "0"}));
}

// git writes the data that only git reads.
void TestCommitGraph::writesWithGit() {
  if (QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("git is not installed");

  mRepo.gitConfig().setValue("commitGraph.generationVersion", 2);
  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  QVERIFY(hasChunk(readGraph(), "GDA2"));
  QVERIFY(!QFile::exists(CommitGraph::path(mRepo) + ".lock"));
  verifyWithGit();

  // It replaces a file that is mapped.
  CommitGraph::reload(mRepo);
  QCOMPARE(walk().size(), 5);
  QVERIFY(commitFile("file.txt", "5").isValid());
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  verifyWithGit();

  CommitGraph::reload(mRepo);
  QCOMPARE(walk(), QStringList({"5", "4", "3", "2", "1", "0"}));
  QVERIFY2(staleFiles().isEmpty(), qPrintable(staleFiles().join(", ")));
}

// git prefers a single file over the split chain of 'git maintenance', so
// writing one would hide the newer chain, including its changed-path filters.
void TestCommitGraph::keepsChainOfGit() {
  if (QStandardPaths::findExecutable("git").isEmpty())
    QSKIP("git is not installed");

  // A single file as written by earlier versions.
  QString path = CommitGraph::path(mRepo);
  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  QByteArray single = readGraph();
  QVERIFY(!single.isEmpty());

  // git replaces it with a chain.
  QString output;
  QVERIFY2(runGit({"commit-graph", "write", "--reachable", "--split=replace",
                   "--changed-paths"},
                  &output),
           qPrintable(output));
  QString chain = QFileInfo(path).dir().filePath(
      "commit-graphs/commit-graph-chain");
  QVERIFY(QFile::exists(chain));
  QVERIFY(!QFile::exists(path));

  // Nothing is written while the chain exists.
  QVERIFY(!CommitGraph::isOutdated(mRepo, 0));
  QVERIFY(!CommitGraph::write(mRepo.dir().path(), &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(!QFile::exists(path));
  QVERIFY(!CommitGraph::removeShadow(mRepo.dir().path()));

  // A file of an earlier version is removed.
  {
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write(single), single.size());
  }
  QVERIFY(CommitGraph::removeShadow(mRepo.dir().path()));
  QVERIFY(!QFile::exists(path));
  QVERIFY(QFile::exists(chain));
  verifyWithGit();
}

namespace {

// Handles (Windows) or mappings of commit-graph files (Linux), -1 if unknown.
int resourceCount() {
#if defined(Q_OS_WIN)
  DWORD count = 0;
  return GetProcessHandleCount(GetCurrentProcess(), &count) ? int(count) : -1;
#elif defined(Q_OS_LINUX)
  QFile maps("/proc/self/maps");
  if (!maps.open(QFile::ReadOnly))
    return -1;
  return maps.readAll().count("commit-graph");
#else
  return -1;
#endif
}

} // namespace

// libgit2 opened an unchanged file again after every refresh of the object
// database, e.g. after looking up a missing object, and leaked the mapping.
void TestCommitGraph::releasesReplacedFile() {
  QString error;
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  CommitGraph::reload(mRepo);
  QCOMPARE(walk().size(), 5);

  int before = resourceCount();
  if (before < 0)
    QSKIP("can't count the resources of the process");

  for (int i = 0; i < 50; ++i) {
    CommitGraph::reload(mRepo);
    QCOMPARE(walk().size(), 5);
  }

  int after = resourceCount();
  QVERIFY2(after - before < 10, qPrintable(QString("%1 -> %2").arg(before).arg(after)));

  // A replaced file is released.
  QVERIFY(commitFile("file.txt", "5").isValid());
  QVERIFY2(CommitGraph::write(mRepo.dir().path(), &error), qPrintable(error));
  CommitGraph::reload(mRepo);
  QCOMPARE(walk().size(), 6);
  CommitGraph::reload(mRepo);
  QVERIFY2(staleFiles().isEmpty(), qPrintable(staleFiles().join(", ")));
}

TEST_MAIN(TestCommitGraph)

#include "commitGraph.moc"
