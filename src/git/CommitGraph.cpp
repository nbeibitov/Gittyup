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
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <memory>
#include <vector>

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

quint32 readBE32(const QByteArray &data, qint64 pos) {
  const uchar *p = reinterpret_cast<const uchar *>(data.constData()) + pos;
  return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) |
         quint32(p[3]);
}

void writeBE32(QByteArray &data, qint64 pos, quint32 value) {
  uchar *p = reinterpret_cast<uchar *>(data.data()) + pos;
  p[0] = uchar(value >> 24);
  p[1] = uchar(value >> 16);
  p[2] = uchar(value >> 8);
  p[3] = uchar(value);
}

// Commit-graph file format, see gitformat-commit-graph(5).
const quint32 kSignature = 0x43475048; // "CGPH"
const quint32 kChunkFanout = 0x4f494446; // "OIDF"
const quint32 kChunkData = 0x43444154; // "CDAT"
const quint32 kChunkEdges = 0x45444745; // "EDGE"
const quint32 kParentNone = 0x70000000;
const quint32 kParentEdge = 0x80000000;
const quint32 kGenerationMax = 0x3fffffff;

} // namespace

// libgit2 computes the generation numbers (topological levels) with a
// traversal that may visit a commit before all of its parents, which yields
// generations lower than those of the parents. git then considers the file
// corrupt, and walks that rely on them can stop too early. Compute them again
// and fix the checksum. Returns false if the data can't be parsed.
bool CommitGraph::fixGenerations(QByteArray &data, bool *changed) {
  *changed = false;
  qint64 size = data.size();
  if (size < 8 || readBE32(data, 0) != kSignature || data.at(4) != 1)
    return false;

  int hashSize;
  QCryptographicHash::Algorithm algorithm;
  switch (data.at(5)) {
    case 1:
      hashSize = 20;
      algorithm = QCryptographicHash::Sha1;
      break;
    case 2:
      hashSize = 32;
      algorithm = QCryptographicHash::Sha256;
      break;
    default:
      return false;
  }

  // Only single files, not split chains.
  int chunks = uchar(data.at(6));
  if (data.at(7) != 0 || size < 8 + (chunks + 1) * 12 + hashSize)
    return false;

  qint64 fanout = -1, commits = -1, edges = -1;
  for (int i = 0; i < chunks; ++i) {
    qint64 entry = 8 + i * 12;
    quint32 id = readBE32(data, entry);
    qint64 offset =
        (qint64(readBE32(data, entry + 4)) << 32) | readBE32(data, entry + 8);
    if (offset < 0 || offset > size - hashSize)
      return false;
    if (id == kChunkFanout)
      fanout = offset;
    else if (id == kChunkData)
      commits = offset;
    else if (id == kChunkEdges)
      edges = offset;
  }

  if (fanout < 0 || commits < 0 || fanout + 256 * 4 > size - hashSize)
    return false;

  qint64 count = readBE32(data, fanout + 255 * 4);
  qint64 entrySize = hashSize + 16;
  if (commits + count * entrySize > size - hashSize)
    return false;

  // Parents of each commit.
  std::vector<std::vector<quint32>> parents(count);
  for (qint64 i = 0; i < count; ++i) {
    qint64 entry = commits + i * entrySize + hashSize;
    quint32 first = readBE32(data, entry);
    quint32 second = readBE32(data, entry + 4);
    if (first != kParentNone)
      parents[i].push_back(first);

    if (second != kParentNone && !(second & kParentEdge)) {
      parents[i].push_back(second);
    } else if (second != kParentNone) {
      // Octopus merge: the other parents are in the edge list.
      if (edges < 0)
        return false;
      for (qint64 e = edges + qint64(second & ~kParentEdge) * 4;; e += 4) {
        if (e + 4 > size - hashSize)
          return false;
        quint32 value = readBE32(data, e);
        parents[i].push_back(value & ~kParentEdge);
        if (value & kParentEdge)
          break;
      }
    }

    for (quint32 parent : parents[i]) {
      if (parent >= count)
        return false;
    }
  }

  // Depth-first, a commit is finished once all its parents are finished.
  enum State : uchar { New, Visiting, Done };
  std::vector<State> states(count, New);
  std::vector<quint32> generations(count, 0);
  std::vector<quint32> stack;
  for (qint64 start = 0; start < count; ++start) {
    if (states[start] == Done)
      continue;

    stack.push_back(start);
    while (!stack.empty()) {
      quint32 i = stack.back();
      if (states[i] == Done) {
        stack.pop_back();
        continue;
      }

      states[i] = Visiting;
      bool ready = true;
      for (quint32 parent : parents[i]) {
        if (states[parent] == Visiting)
          return false; // a cycle
        if (states[parent] == New) {
          stack.push_back(parent);
          ready = false;
        }
      }

      if (!ready)
        continue;

      quint32 generation = 0;
      for (quint32 parent : parents[i])
        generation = qMax(generation, generations[parent]);
      generations[i] = qMin(generation + 1, kGenerationMax);
      states[i] = Done;
      stack.pop_back();
    }
  }

  // The generation shares a word with the two high bits of the time.
  for (qint64 i = 0; i < count; ++i) {
    qint64 pos = commits + i * entrySize + hashSize + 8;
    quint32 word = readBE32(data, pos);
    quint32 fixed = (generations[i] << 2) | (word & 0x3);
    if (fixed != word) {
      writeBE32(data, pos, fixed);
      *changed = true;
    }
  }

  if (*changed) {
    QByteArray hash = QCryptographicHash::hash(
        QByteArrayView(data.constData(), size - hashSize), algorithm);
    data.replace(size - hashSize, hashSize, hash);
  }

  return true;
}

bool CommitGraph::isCorrupt(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;

  QByteArray data = file.readAll();
  bool changed = false;
  return fixGenerations(data, &changed) && changed;
}

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

  QByteArray data(buf.ptr, static_cast<qsizetype>(buf.size));
  git_buf_dispose(&buf);

  bool changed = false;
  if (!fixGenerations(data, &changed))
    return fail(QObject::tr("Unable to check the commit-graph."));

  bool written = (lock.write(data) == data.size());
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
