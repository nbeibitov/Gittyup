//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "CommitGraph.h"
#include "Config.h"
#include "git2.h"
#include "git2/sys/commit_graph.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <memory>

namespace git {

namespace {

const QString kFile = "commit-graph";
const QString kLock = "commit-graph.lock";
const QString kStaleSuffix = ".stale";

// A lock older than this was left behind by a crashed writer.
const int kLockTimeout = 3600;

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  if (error && error->message && *error->message)
    return QString::fromUtf8(error->message);
  return fallback;
}

QString infoDir(git_repository *repo) {
  git_buf buf = GIT_BUF_INIT;
  if (git_repository_item_path(&buf, repo, GIT_REPOSITORY_ITEM_OBJECTS))
    return QString();

  QString objects = QString::fromUtf8(buf.ptr, static_cast<int>(buf.size));
  git_buf_dispose(&buf);
  return QDir(objects).filePath("info");
}

// Replaced files that were still mapped when they were replaced.
void removeStale(const QString &info) {
  QDir dir(info);
  const QStringList names =
      dir.entryList({kFile + "-*" + kStaleSuffix}, QDir::Files);
  for (const QString &name : names)
    dir.remove(name);
}

qint64 modified(const QString &path) {
  QFileInfo info(path);
  return info.exists() ? info.lastModified().toSecsSinceEpoch() : 0;
}

} // namespace

QString CommitGraph::path(const Repository &repo) {
  QString info = infoDir(repo);
  return info.isEmpty() ? QString() : QDir(info).filePath(kFile);
}

bool CommitGraph::isOutdated(const Repository &repo, int minAge) {
  if (!repo.isValid() ||
      !repo.gitConfig().value<bool>("core.commitGraph", true))
    return false;

  QString file = path(repo);
  if (file.isEmpty())
    return false;

  qint64 graph = modified(file);
  if (!graph)
    return true;

  // References change on commits, checkouts and fetches.
  QDir dir = repo.dir();
  qint64 refs = 0;
  for (const QString &name : {"packed-refs", "logs/HEAD", "FETCH_HEAD"})
    refs = qMax(refs, modified(dir.filePath(name)));

  qint64 now = QDateTime::currentSecsSinceEpoch();
  return refs > graph && now - graph >= minAge;
}

bool CommitGraph::write(const QString &gitDir, QString *error) {
  auto fail = [error](const QString &text) {
    if (error)
      *error = text;
    return false;
  };

  git_repository *raw = nullptr;
  if (git_repository_open_ext(&raw, gitDir.toUtf8(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr))
    return fail(lastError(QObject::tr("Unable to open the repository.")));

  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(
      raw, git_repository_free);

  QString info = infoDir(repo.get());
  if (info.isEmpty() || !QDir().mkpath(info))
    return fail(QObject::tr("Unable to find the objects directory."));

  removeStale(info);

  // Take the lock git uses for writing the commit-graph.
  QDir dir(info);
  QString lockPath = dir.filePath(kLock);
  qint64 now = QDateTime::currentSecsSinceEpoch();
  qint64 locked = modified(lockPath);
  if (locked && now - locked > kLockTimeout)
    QFile::remove(lockPath);

  QFile lock(lockPath);
  if (!lock.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return fail(QObject::tr("The commit-graph is locked by another process."));

  // Remove the lock on every failure below.
  bool done = false;
  auto unlock = [&lock, &lockPath, &done](void *) {
    if (done)
      return;
    lock.close();
    QFile::remove(lockPath);
  };
  std::unique_ptr<void, decltype(unlock)> guard(&lock, unlock);

  git_commit_graph_writer_options opts = GIT_COMMIT_GRAPH_WRITER_OPTIONS_INIT;
  git_commit_graph_writer *writer = nullptr;
  if (git_commit_graph_writer_new(&writer, info.toUtf8(), &opts))
    return fail(lastError(QObject::tr("Unable to create the commit-graph.")));

  std::unique_ptr<git_commit_graph_writer,
                  decltype(&git_commit_graph_writer_free)>
      writerGuard(writer, git_commit_graph_writer_free);

  git_revwalk *walk = nullptr;
  if (git_revwalk_new(&walk, repo.get()))
    return fail(lastError(QObject::tr("Unable to walk the history.")));

  std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walkGuard(
      walk, git_revwalk_free);

  // All references like 'git commit-graph write --reachable', and HEAD.
  if (git_revwalk_push_glob(walk, "refs/*"))
    return fail(lastError(QObject::tr("Unable to walk the history.")));
  git_revwalk_push_head(walk); // fails for an unborn HEAD

  if (git_commit_graph_writer_add_revwalk(writer, walk))
    return fail(lastError(QObject::tr("Unable to walk the history.")));

  git_buf buf = GIT_BUF_INIT;
  if (git_commit_graph_writer_dump(&buf, writer))
    return fail(lastError(QObject::tr("Unable to create the commit-graph.")));

  qint64 size = static_cast<qint64>(buf.size);
  bool written = (lock.write(buf.ptr, size) == size);
  git_buf_dispose(&buf);
  lock.close();
  if (!written || lock.error() != QFileDevice::NoError)
    return fail(QObject::tr("Unable to write the commit-graph."));

  // A memory mapped file can't be replaced or deleted on Windows, but it can
  // be renamed. Move the old file aside and remove it once it is unmapped.
  QString target = dir.filePath(kFile);
  QString stale;
  if (QFile::exists(target)) {
    stale = dir.filePath(QString("%1-%2%3")
                             .arg(kFile)
                             .arg(QDateTime::currentMSecsSinceEpoch())
                             .arg(kStaleSuffix));
    if (!QFile::rename(target, stale))
      return fail(QObject::tr("Unable to replace the commit-graph."));
  }

  if (!QFile::rename(lockPath, target)) {
    if (!stale.isEmpty())
      QFile::rename(stale, target);
    return fail(QObject::tr("Unable to replace the commit-graph."));
  }

  done = true;
  if (!stale.isEmpty())
    QFile::remove(stale); // fails while it is still mapped

  return true;
}

void CommitGraph::reload(const Repository &repo) {
  if (!repo.isValid())
    return;

  // The commit-graph of the object database is reloaded if it changed.
  git_odb *odb = nullptr;
  if (!git_repository_odb(&odb, repo)) {
    git_odb_refresh(odb);
    git_odb_free(odb);
  }

  QString info = infoDir(repo);
  if (!info.isEmpty())
    removeStale(info);
}

} // namespace git
