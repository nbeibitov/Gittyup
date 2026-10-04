//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef INDEXREFRESH_H
#define INDEXREFRESH_H

#include <QStringList>
#include <atomic>
#include <optional>

namespace git {

// Updates the stat information (size, times) that the index keeps for each
// file, like 'git status' and 'git update-index --refresh' do. Files whose
// stat information differs from the index are read completely by every
// status check, which takes minutes in large working trees after a tool
// touched all files. libgit2 only updates the index during a diff if asked
// to, and then writes it right away; the status check of Gittyup can't do
// that because the index is shared with the GUI thread.
class IndexRefresh {
public:
  struct Result {
    bool written = false; // the index was updated
    int hashed = 0;       // files that were read, -1 if unknown
    QString error;
  };

  // Opens its own repository handle, so it can run on any thread. The
  // index is updated on a copy and only replaced if it didn't change in the
  // meantime, e.g. by staging files. Setting canceled stops it.
  //
  // libgit2 drops the index extensions it doesn't know when it writes the
  // index, e.g. the untracked cache and the fsmonitor data of git, which then
  // makes 'git status' slow. Such an index is refreshed by git instead, and
  // isn't changed if git isn't installed.
  static Result run(const QString &gitDir,
                    const std::atomic<bool> *canceled = nullptr);

  // The signatures of the extensions of index data, or nothing if the data
  // can't be parsed.
  static std::optional<QStringList> extensions(const QByteArray &index);
};

} // namespace git

#endif
