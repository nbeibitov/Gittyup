//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef COMMITGRAPH_H
#define COMMITGRAPH_H

#include "Repository.h"
#include <QByteArray>
#include <QString>

namespace git {

// Maintains objects/info/commit-graph, which speeds up walking the history.
// libgit2 only reads this single file, not the split chains that
// 'git maintenance' and fetch.writeCommitGraph write by default. git prefers
// the single file, so it isn't written while such a chain exists.
class CommitGraph {
public:
  // True if the commit-graph file is missing, or older than minAge seconds
  // while references changed after it was written. False if disabled with
  // core.commitGraph or if git maintains a split chain.
  static bool isOutdated(const Repository &repo, int minAge = 3600);

  // Write the commit-graph of all commits reachable from references. Uses
  // git if it is installed, which also writes the data that only git reads
  // (corrected commit dates and, if enabled, changed-path filters), and
  // libgit2 otherwise. Opens its own repository handle, so it can run on any
  // thread. The file is replaced in a way that works on Windows while it is
  // memory mapped. Fails if git maintains a split chain.
  static bool write(const QString &gitDir, QString *error = nullptr);

  // Remove a single file that hides a split chain from git. Earlier versions
  // wrote one regardless of the chain. Returns true if a file was removed.
  static bool removeShadow(const QString &gitDir);

  // Make repo use a new commit-graph file and remove the replaced ones.
  static void reload(const Repository &repo);

  // True if the commit-graph file has wrong generation numbers, as written
  // by the libgit2 writer for histories with merges.
  static bool isCorrupt(const QString &path);

  // Recompute the generation numbers of commit-graph data and update its
  // checksum. Returns false if the data can't be parsed.
  static bool fixGenerations(QByteArray &data, bool *changed);

  static QString path(const Repository &repo);
};

} // namespace git

#endif
