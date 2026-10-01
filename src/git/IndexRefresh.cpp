//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "IndexRefresh.h"
#include "git2.h"
#include "git2/sys/diff.h"
#include "git2/sys/repository.h"
#include <QDir>
#include <QFile>
#include <QObject>
#include <filesystem>
#include <memory>

namespace git {

namespace {

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  if (error && error->message && *error->message)
    return QString::fromUtf8(error->message);
  return fallback;
}

bool readFile(const QString &path, QByteArray &content) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  content = file.readAll();
  return file.error() == QFileDevice::NoError;
}

int progress(const git_diff *, const char *, const char *, void *payload) {
  auto canceled = static_cast<const std::atomic<bool> *>(payload);
  return (canceled && *canceled) ? -1 : 0;
}

bool writeFile(QFile &file, const QByteArray &content) {
  bool written = (file.write(content) == content.size());
  file.close();
  return written && file.error() == QFileDevice::NoError;
}

} // namespace

IndexRefresh::Result IndexRefresh::run(const QString &gitDir,
                                       const std::atomic<bool> *canceled) {
  Result result;
  auto fail = [&result](const QString &error) {
    result.error = error;
    return result;
  };

  git_repository *raw = nullptr;
  if (git_repository_open_ext(&raw, gitDir.toUtf8(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr))
    return fail(lastError(QObject::tr("Unable to open the repository.")));

  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(
      raw, git_repository_free);

  if (git_repository_is_bare(repo.get()))
    return result;

  git_buf buf = GIT_BUF_INIT;
  if (git_repository_item_path(&buf, repo.get(), GIT_REPOSITORY_ITEM_INDEX))
    return fail(lastError(QObject::tr("Unable to find the index.")));
  QString indexPath = QString::fromUtf8(buf.ptr, static_cast<int>(buf.size));
  git_buf_dispose(&buf);

  QByteArray original;
  if (!QFile::exists(indexPath))
    return result;
  if (!readFile(indexPath, original))
    return fail(QObject::tr("Unable to read the index."));

  // Work on a copy. libgit2 updates the repository's index during the diff
  // and writes it at the end, without checking whether the file changed.
  QDir dir(QString::fromUtf8(git_repository_path(repo.get())));
  if (!dir.mkpath("gittyup"))
    return fail(QObject::tr("Unable to create a temporary index."));
  QString copyPath = dir.filePath("gittyup/index-refresh");
  {
    QFile copy(copyPath);
    if (!copy.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        !writeFile(copy, original))
      return fail(QObject::tr("Unable to create a temporary index."));
  }

  auto removeCopy = [&copyPath](void *) { QFile::remove(copyPath); };
  std::unique_ptr<void, decltype(removeCopy)> copyGuard(&copyPath, removeCopy);

  git_index *index = nullptr;
  if (git_index_open(&index, copyPath.toUtf8()))
    return fail(lastError(QObject::tr("Unable to read the index.")));
  std::unique_ptr<git_index, decltype(&git_index_free)> indexGuard(
      index, git_index_free);

  if (git_repository_set_index(repo.get(), index))
    return fail(lastError(QObject::tr("Unable to read the index.")));

  // Only tracked files matter, untracked ones have no stat information.
  git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
  opts.flags = GIT_DIFF_UPDATE_INDEX | GIT_DIFF_INCLUDE_TYPECHANGE;
  opts.ignore_submodules = GIT_SUBMODULE_IGNORE_ALL;
  opts.progress_cb = &progress;
  opts.payload = const_cast<std::atomic<bool> *>(canceled);

  git_diff *diff = nullptr;
  if (git_diff_index_to_workdir(&diff, repo.get(), index, &opts))
    return fail(lastError(QObject::tr("Unable to compare the index.")));

  git_diff_perfdata perf = GIT_DIFF_PERFDATA_INIT;
  if (!git_diff_get_perfdata(&perf, diff))
    result.hashed = static_cast<int>(perf.oid_calculations);
  git_diff_free(diff);

  QByteArray updated;
  if (!readFile(copyPath, updated))
    return fail(QObject::tr("Unable to read the updated index."));
  if (updated == original)
    return result; // nothing to update

  // Replace the index with the lock git uses, unless it changed meanwhile.
  QString lockPath = indexPath + ".lock";
  QFile lock(lockPath);
  if (!lock.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return fail(QObject::tr("The index is locked by another process."));

  QByteArray current;
  if (!readFile(indexPath, current) || current != original) {
    lock.close();
    QFile::remove(lockPath);
    return fail(QObject::tr("The index changed while it was refreshed."));
  }

  if (!writeFile(lock, updated)) {
    QFile::remove(lockPath);
    return fail(QObject::tr("Unable to write the index."));
  }

  // Replaces the existing file (MoveFileEx with MOVEFILE_REPLACE_EXISTING
  // on Windows).
  std::error_code error;
  std::filesystem::rename(std::filesystem::path(lockPath.toStdU16String()),
                          std::filesystem::path(indexPath.toStdU16String()),
                          error);
  if (error) {
    QFile::remove(lockPath);
    return fail(QObject::tr("Unable to replace the index."));
  }

  result.written = true;
  return result;
}

} // namespace git
