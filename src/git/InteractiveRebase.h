//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef INTERACTIVEREBASE_H
#define INTERACTIVEREBASE_H

#include "Commit.h"
#include "Id.h"
#include "Repository.h"
#include <QCoreApplication>
#include <QList>
#include <QString>

namespace git {

// Interactive rebase implemented on top of libgit2 (which only supports
// plain pick rebases). Commits are applied one by one with cherry-pick onto
// a detached HEAD. The plan is persisted in .git/rebase-merge together with
// the files git itself uses, so that the repository reports an interactive
// rebase in progress and `git rebase --abort` works from the command line.
class InteractiveRebase {
  Q_DECLARE_TR_FUNCTIONS(InteractiveRebase)

public:
  enum class Action { Pick, Reword, Edit, Squash, Fixup, Drop };

  struct Step {
    Action action = Action::Pick;
    Id commit;
    // New message for Reword and Squash. Empty means the default message
    // (original message for Reword, combined messages for Squash).
    QString message;
  };

  struct Options {
    bool autostash = false;
    bool keepEmpty = false;
    bool committerDateIsAuthorDate = false;
    // Committer override. Empty means the repository default signature.
    QString committerName;
    QString committerEmail;
    // Commit to apply the steps onto. Invalid means the base commit, i.e.
    // the commits are rewritten in place.
    Id onto;
  };

  enum class Status { Finished, Conflict, Edit, Error };

  // What happened to a single step while running.
  struct Applied {
    enum Kind { Rewritten, Unchanged, Dropped, Empty };

    int step = -1;
    Action action = Action::Pick;
    Kind kind = Rewritten;
    Id before;
    Id after;
  };

  struct Result {
    Status status = Status::Error;
    QString error;
    int step = -1; // step that stopped the rebase
    QList<Applied> applied;
  };

  InteractiveRebase(const Repository &repo);

  // True if an interactive rebase started by Gittyup is in progress.
  static bool isInProgress(const Repository &repo);

  // Commits after base up to HEAD, oldest first. Merge commits are not
  // supported; an error is reported if the range contains one.
  static QList<Commit> commits(const Repository &repo, const Commit &base,
                               QString *error = nullptr);

  // Default steps (all pick) for the given commits.
  static QList<Step> defaultSteps(const QList<Commit> &commits);

  // Move "fixup! <subject>" and "squash! <subject>" commits behind the commit
  // they refer to and set the matching action.
  static QList<Step> autosquash(const Repository &repo,
                                const QList<Step> &steps);

  // Returns an error message if the steps cannot be executed.
  static QString validate(const QList<Step> &steps);

  static QString actionName(Action action);
  static bool actionFromName(const QString &name, Action &action);

  // Start rebasing the commits after base (exclusive) up to HEAD following
  // the steps, onto options.onto or base.
  Result start(const Commit &base, const QList<Step> &steps,
               const Options &options);

  // Continue after resolving conflicts or after an edit stop. The message is
  // used for the commit that stopped with conflicts. Empty means default.
  Result resume(const QString &message = QString());

  // Skip the step that stopped with conflicts and continue.
  Result skip();

  // Restore the branch and working tree to the state before the rebase.
  bool abort(QString *error = nullptr);

  // State of an ongoing rebase.
  QList<Step> steps() const;
  int stepCount() const;
  bool isStoppedAtConflict() const;
  // Message proposed for the commit that stopped with conflicts.
  QString pendingMessage() const;

private:
  struct State;

  bool load(State &state) const;
  bool save(const State &state) const;
  void removeState() const;
  QString statePath(const QString &file = QString()) const;

  Result run(State &state, Result result);
  bool apply(State &state, int index, Result &result);
  bool commitStep(State &state, int index, const QString &message,
                  Result &result);
  Result finish(State &state, Result result);
  QString defaultMessage(const State &state, int index) const;
  bool restoreAutostash(const State &state, QString *error);
  bool isWorkdirDirty() const;

  Repository mRepo;
};

} // namespace git

#endif
