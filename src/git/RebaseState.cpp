//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "RebaseState.h"
#include "InteractiveRebase.h"
#include "Rebase.h"
#include <QDir>
#include <QFile>

namespace git {

namespace {

QString readFile(const QDir &dir, const QString &name) {
  QFile file(dir.filePath(name));
  if (!file.open(QIODevice::ReadOnly))
    return QString();
  return QString::fromUtf8(file.readAll()).trimmed();
}

// Number of commands in a todo list (git-rebase-todo or done).
int countCommands(const QDir &dir, const QString &name) {
  int count = 0;
  const QStringList lines = readFile(dir, name).split('\n');
  for (const QString &line : lines) {
    QString trimmed = line.trimmed();
    if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
      ++count;
  }
  return count;
}

} // namespace

RebaseState RebaseState::read(const Repository &repo) {
  RebaseState state;
  if (!repo.isValid())
    return state;

  QDir gitDir = repo.dir();
  QDir dir;
  bool apply = false;
  if (gitDir.exists("rebase-merge")) {
    dir = QDir(gitDir.filePath("rebase-merge"));
  } else if (gitDir.exists("rebase-apply") &&
             QFile::exists(gitDir.filePath("rebase-apply/rebasing"))) {
    dir = QDir(gitDir.filePath("rebase-apply"));
    apply = true;
  } else {
    return state;
  }

  QString headName = readFile(dir, "head-name");
  if (headName.startsWith("refs/heads/"))
    state.mBranch = headName.mid(QString("refs/heads/").length());

  QString onto = readFile(dir, "onto");
  if (!onto.isEmpty())
    state.mOnto = Id(QByteArray::fromHex(onto.toUtf8()), repo.oidType());

  // Written by libgit2 and some tools, but not by the git command line.
  state.mOntoName = readFile(dir, "onto_name");

  if (apply) {
    state.mDone = readFile(dir, "next").toInt();
    state.mTotal = readFile(dir, "last").toInt();
  } else if (dir.exists("git-rebase-todo") || dir.exists("done")) {
    state.mDone = countCommands(dir, "done");
    state.mTotal = state.mDone + countCommands(dir, "git-rebase-todo");
  } else {
    state.mDone = readFile(dir, "msgnum").toInt();
    state.mTotal = readFile(dir, "end").toInt();
  }

  if (InteractiveRebase::isInProgress(repo)) {
    state.mOwner = Owner::Gittyup;
  } else {
    Repository copy = repo;
    state.mOwner =
        copy.rebaseOpen().isValid() ? Owner::Libgit2 : Owner::External;
  }

  return state;
}

} // namespace git
