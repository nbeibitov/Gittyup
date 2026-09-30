//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef REBASESTATE_H
#define REBASESTATE_H

#include "Id.h"
#include "Repository.h"
#include <QString>

namespace git {

// Description of a rebase in progress, read from the state files that git
// keeps in .git/rebase-merge or .git/rebase-apply. Works for rebases started
// by Gittyup as well as by the git command line or other tools.
class RebaseState {
public:
  enum class Owner {
    None,     // no rebase in progress
    Gittyup,  // interactive rebase started by Gittyup
    Libgit2,  // plain rebase that libgit2 can continue
    External  // anything else, handled with the git command line
  };

  static RebaseState read(const Repository &repo);

  bool isValid() const { return mOwner != Owner::None; }
  Owner owner() const { return mOwner; }

  // Short name of the rebased branch. Empty for a detached HEAD.
  QString branch() const { return mBranch; }
  Id onto() const { return mOnto; }
  // Reference name (e.g. refs/heads/master) of the upstream the rebase was
  // started with, if recorded.
  QString ontoName() const { return mOntoName; }

  // Number of processed and total steps. Zero total if unknown.
  int done() const { return mDone; }
  int total() const { return mTotal; }

private:
  Owner mOwner = Owner::None;
  QString mBranch;
  Id mOnto;
  QString mOntoName;
  int mDone = 0;
  int mTotal = 0;
};

} // namespace git

#endif
