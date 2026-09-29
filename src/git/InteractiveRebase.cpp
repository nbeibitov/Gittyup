//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "InteractiveRebase.h"
#include "Reference.h"
#include "git2.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <memory>

namespace git {

namespace {

const QString kStateDir = "rebase-merge";
const QString kPlanFile = "gittyup-plan.json";
const QString kDetachedHead = "detached HEAD";
const int kPlanVersion = 1;

const QStringList kActionNames = {"pick",   "reword", "edit",
                                  "squash", "fixup",  "drop"};

using CommitPtr = std::unique_ptr<git_commit, decltype(&git_commit_free)>;
using TreePtr = std::unique_ptr<git_tree, decltype(&git_tree_free)>;
using IndexPtr = std::unique_ptr<git_index, decltype(&git_index_free)>;
using SignaturePtr =
    std::unique_ptr<git_signature, decltype(&git_signature_free)>;
using ReferencePtr =
    std::unique_ptr<git_reference, decltype(&git_reference_free)>;

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  if (error && error->message && *error->message)
    return QString::fromUtf8(error->message);
  return fallback;
}

CommitPtr lookupCommit(git_repository *repo, const git_oid *id) {
  git_commit *commit = nullptr;
  if (id)
    git_commit_lookup(&commit, repo, id);
  return CommitPtr(commit, git_commit_free);
}

CommitPtr headCommit(git_repository *repo) {
  git_oid id;
  if (git_reference_name_to_id(&id, repo, "HEAD"))
    return CommitPtr(nullptr, git_commit_free);
  return lookupCommit(repo, &id);
}

QString commitMessage(const git_commit *commit) {
  return QString::fromUtf8(git_commit_message(commit));
}

// Normalize whitespace like git does (without stripping comments).
QByteArray prettify(const QString &message) {
  QByteArray raw = message.toUtf8();
  git_buf buf = GIT_BUF_INIT;
  if (git_message_prettify(&buf, raw.constData(), 0, '#'))
    return raw;

  QByteArray result(buf.ptr, static_cast<int>(buf.size));
  git_buf_dispose(&buf);
  return result;
}

bool writeFile(const QString &path, const QByteArray &content) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(content);
  return file.commit();
}

// git_cherrypick() leaves the repository in cherry-pick state. The rebase
// state takes precedence, but remove the files to not confuse other tools.
void removeCherryPickState(git_repository *repo) {
  QDir dir(QString::fromUtf8(git_repository_path(repo)));
  dir.remove("CHERRY_PICK_HEAD");
  dir.remove("MERGE_MSG");
}

// Actions that take a message from the plan.
bool hasMessage(InteractiveRebase::Action action) {
  return action == InteractiveRebase::Action::Reword ||
         action == InteractiveRebase::Action::Squash;
}

bool checkoutCommit(git_repository *repo, git_commit *commit) {
  git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
  opts.checkout_strategy = GIT_CHECKOUT_SAFE;
  if (git_checkout_tree(repo, reinterpret_cast<git_object *>(commit), &opts))
    return false;
  return !git_repository_set_head_detached(repo, git_commit_id(commit));
}

} // namespace

struct InteractiveRebase::State {
  QList<Step> steps;
  Options options;
  int next = 0;          // index of the next step to process
  bool conflict = false; // steps[next] stopped with conflicts
  QString headName;      // branch reference name or kDetachedHead
  Id origHead;
  Id onto;
  Id autostash; // stash commit created by autostash
};

InteractiveRebase::InteractiveRebase(const Repository &repo) : mRepo(repo) {}

bool InteractiveRebase::isInProgress(const Repository &repo) {
  if (!repo.isValid())
    return false;

  QDir dir(QString::fromUtf8(git_repository_path(repo)));
  return dir.exists(kStateDir + "/" + kPlanFile);
}

QList<Commit> InteractiveRebase::commits(const Repository &repo,
                                         const Commit &base, QString *error) {
  git_revwalk *walker = nullptr;
  if (git_revwalk_new(&walker, repo))
    return {};

  std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> guard(
      walker, git_revwalk_free);
  git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  if (git_revwalk_push_head(walker))
    return {};

  if (base.isValid() && git_revwalk_hide(walker, base.id()))
    return {};

  QList<Commit> result;
  git_oid id;
  while (!git_revwalk_next(&id, walker)) {
    Commit commit = repo.lookupCommit(Id(id));
    if (!commit.isValid())
      return {};

    if (commit.parents().size() != 1) {
      if (error)
        *error = commit.isMerge()
                     ? tr("Merge commits cannot be rebased interactively "
                          "(%1).")
                           .arg(commit.shortId())
                     : tr("The root commit cannot be rebased interactively.");
      return {};
    }

    result.append(commit);
  }

  return result;
}

QList<InteractiveRebase::Step>
InteractiveRebase::defaultSteps(const QList<Commit> &commits) {
  QList<Step> steps;
  for (const Commit &commit : commits) {
    Step step;
    step.commit = commit.id();
    steps.append(step);
  }
  return steps;
}

QList<InteractiveRebase::Step>
InteractiveRebase::autosquash(const Repository &repo,
                              const QList<Step> &steps) {
  int count = steps.size();
  QStringList subjects;
  for (const Step &step : steps)
    subjects.append(repo.lookupCommit(step.commit).summary());

  QVector<int> root(count, -1);
  QVector<Action> actions(count);
  QMap<int, QList<int>> children;

  for (int j = 0; j < count; ++j) {
    QString subject = subjects.at(j);
    bool marked = false;
    Action action = Action::Fixup;
    while (true) {
      bool squash = subject.startsWith("squash! ");
      bool fixup =
          subject.startsWith("fixup! ") || subject.startsWith("amend! ");
      if (!squash && !fixup)
        break;

      if (!marked)
        action = squash ? Action::Squash : Action::Fixup;
      marked = true;
      subject = subject.mid(subject.indexOf(' ') + 1);
    }

    if (!marked)
      continue;

    // Prefer an exact subject match, then a prefix match.
    int target = -1;
    for (int i = 0; i < j && target < 0; ++i) {
      if (subjects.at(i) == subject)
        target = i;
    }
    for (int i = 0; i < j && target < 0; ++i) {
      if (subjects.at(i).startsWith(subject))
        target = i;
    }

    if (target < 0)
      continue;

    if (root.at(target) >= 0)
      target = root.at(target);

    root[j] = target;
    actions[j] = action;
    children[target].append(j);
  }

  QList<Step> result;
  for (int i = 0; i < count; ++i) {
    if (root.at(i) >= 0)
      continue;

    result.append(steps.at(i));
    for (int child : children.value(i)) {
      Step step = steps.at(child);
      step.action = actions.at(child);
      result.append(step);
    }
  }

  return result;
}

QString InteractiveRebase::validate(const QList<Step> &steps) {
  if (steps.isEmpty())
    return tr("There are no commits to rebase.");

  QSet<QString> ids;
  bool previous = false;
  for (const Step &step : steps) {
    QString id = step.commit.toString();
    if (ids.contains(id))
      return tr("A commit appears more than once.");
    ids.insert(id);

    switch (step.action) {
      case Action::Drop:
        break;

      case Action::Squash:
      case Action::Fixup:
        if (!previous)
          return tr("Cannot '%1' without a previous commit.")
              .arg(actionName(step.action));
        break;

      default:
        previous = true;
        break;
    }
  }

  return QString();
}

QString InteractiveRebase::actionName(Action action) {
  return kActionNames.at(static_cast<int>(action));
}

bool InteractiveRebase::actionFromName(const QString &name, Action &action) {
  int index = kActionNames.indexOf(name);
  if (index < 0)
    return false;

  action = static_cast<Action>(index);
  return true;
}

InteractiveRebase::Result InteractiveRebase::start(const Commit &base,
                                                   const QList<Step> &steps,
                                                   const Options &options) {
  Result result;
  git_repository *repo = mRepo;

  if (mRepo.state() != GIT_REPOSITORY_STATE_NONE) {
    result.error = tr("Another operation (merge, rebase, cherry-pick, ...) is "
                      "in progress.");
    return result;
  }

  QString error = validate(steps);
  if (!error.isEmpty()) {
    result.error = error;
    return result;
  }

  for (const Step &step : steps) {
    CommitPtr commit = lookupCommit(repo, step.commit);
    if (!commit) {
      result.error = tr("Commit %1 not found.").arg(step.commit.toString());
      return result;
    }

    if (git_commit_parentcount(commit.get()) != 1) {
      result.error = tr("Merge and root commits cannot be rebased "
                        "interactively.");
      return result;
    }
  }

  git_reference *ref = nullptr;
  if (git_repository_head(&ref, repo)) {
    result.error = lastError(tr("Invalid HEAD."));
    return result;
  }

  ReferencePtr head(ref, git_reference_free);
  State state;
  state.steps = steps;
  state.options = options;
  state.onto = options.onto.isValid() ? options.onto : base.id();
  state.origHead = Id(git_reference_target(head.get()));
  state.headName = git_reference_is_branch(head.get())
                       ? QString::fromUtf8(git_reference_name(head.get()))
                       : kDetachedHead;

  if (state.origHead != base.id() &&
      git_graph_descendant_of(repo, state.origHead, base.id()) != 1) {
    result.error = tr("The base commit is not an ancestor of HEAD.");
    return result;
  }

  if (!lookupCommit(repo, state.onto)) {
    result.error = tr("Commit %1 not found.").arg(state.onto.toString());
    return result;
  }

  if (isWorkdirDirty()) {
    if (!options.autostash) {
      result.error = tr("You have uncommitted changes. Commit or stash them, "
                        "or enable autostash.");
      return result;
    }

    Commit stash = mRepo.stash(tr("autostash"));
    if (!stash.isValid()) {
      result.error = lastError(tr("Unable to stash uncommitted changes."));
      return result;
    }

    state.autostash = stash.id();
  }

  // Leading picks whose parent is already in place are kept unchanged.
  Id current = state.onto;
  while (state.next < steps.size()) {
    const Step &step = steps.at(state.next);
    CommitPtr commit = lookupCommit(repo, step.commit);
    if (step.action != Action::Pick ||
        Id(git_commit_parent_id(commit.get(), 0)) != current)
      break;

    Applied applied;
    applied.step = state.next;
    applied.action = step.action;
    applied.kind = Applied::Unchanged;
    applied.before = step.commit;
    applied.after = step.commit;
    result.applied.append(applied);

    current = step.commit;
    ++state.next;
  }

  CommitPtr target = lookupCommit(repo, current);
  if (!target || !checkoutCommit(repo, target.get())) {
    result.error = lastError(
        tr("Unable to check out %1.").arg(current.toString().left(7)));
    restoreAutostash(state, nullptr);
    return result;
  }

  if (!save(state)) {
    result.error = tr("Unable to write the rebase state.");
    return result;
  }

  emit mRepo.notifier()->stateChanged();
  return run(state, result);
}

InteractiveRebase::Result InteractiveRebase::resume(const QString &message) {
  Result result;
  State state;
  if (!load(state)) {
    result.error = tr("No interactive rebase in progress.");
    return result;
  }

  if (state.conflict) {
    IndexPtr index(nullptr, git_index_free);
    git_index *raw = nullptr;
    if (!git_repository_index(&raw, mRepo))
      index.reset(raw);

    if (!index || git_index_read(index.get(), 0)) {
      result.error = lastError(tr("Unable to read the index."));
      return result;
    }

    if (git_index_has_conflicts(index.get())) {
      result.step = state.next;
      result.error = tr("Resolve all conflicts and stage the files before "
                        "continuing.");
      return result;
    }

    if (!commitStep(state, state.next, message, result)) {
      save(state);
      emit mRepo.notifier()->referenceUpdated(mRepo.head());
      return result;
    }
  }

  if (isWorkdirDirty()) {
    result.step = state.next;
    result.error = tr("You have uncommitted changes. Commit (or amend) them "
                      "before continuing.");
    return result;
  }

  return run(state, result);
}

InteractiveRebase::Result InteractiveRebase::skip() {
  Result result;
  State state;
  if (!load(state)) {
    result.error = tr("No interactive rebase in progress.");
    return result;
  }

  git_repository *repo = mRepo;
  CommitPtr head = headCommit(repo);
  git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
  if (!head || git_reset(repo, reinterpret_cast<git_object *>(head.get()),
                         GIT_RESET_HARD, &opts)) {
    result.error = lastError(tr("Unable to reset to HEAD."));
    return result;
  }

  removeCherryPickState(repo);
  if (state.next < state.steps.size()) {
    const Step &step = state.steps.at(state.next);
    Applied applied;
    applied.step = state.next;
    applied.action = step.action;
    applied.kind = Applied::Dropped;
    applied.before = step.commit;
    result.applied.append(applied);
    ++state.next;
  }

  state.conflict = false;
  save(state);
  return run(state, result);
}

bool InteractiveRebase::abort(QString *error) {
  State state;
  if (!load(state)) {
    if (error)
      *error = tr("No interactive rebase in progress.");
    return false;
  }

  git_repository *repo = mRepo;
  CommitPtr orig = lookupCommit(repo, state.origHead);
  if (!orig) {
    if (error)
      *error = tr("The original HEAD commit was not found.");
    return false;
  }

  QByteArray headName = state.headName.toUtf8();
  int result = (state.headName == kDetachedHead)
                   ? git_repository_set_head_detached(repo, state.origHead)
                   : git_repository_set_head(repo, headName.constData());
  git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
  if (result || git_reset(repo, reinterpret_cast<git_object *>(orig.get()),
                          GIT_RESET_HARD, &opts)) {
    if (error)
      *error = lastError(tr("Unable to restore the original HEAD."));
    return false;
  }

  removeCherryPickState(repo);
  removeState();

  restoreAutostash(state, error);
  emit mRepo.notifier()->referenceUpdated(mRepo.head());
  emit mRepo.notifier()->stateChanged();
  return true;
}

QList<InteractiveRebase::Step> InteractiveRebase::steps() const {
  State state;
  return load(state) ? state.steps : QList<Step>();
}

int InteractiveRebase::stepCount() const { return steps().size(); }

bool InteractiveRebase::isStoppedAtConflict() const {
  State state;
  return load(state) && state.conflict;
}

QString InteractiveRebase::pendingMessage() const {
  State state;
  if (!load(state) || state.next >= state.steps.size())
    return QString();
  return defaultMessage(state, state.next);
}

InteractiveRebase::Result InteractiveRebase::run(State &state, Result result) {
  while (state.next < state.steps.size()) {
    if (!apply(state, state.next, result)) {
      save(state);
      emit mRepo.notifier()->referenceUpdated(mRepo.head());
      return result;
    }
  }

  return finish(state, result);
}

bool InteractiveRebase::apply(State &state, int index, Result &result) {
  git_repository *repo = mRepo;
  const Step step = state.steps.at(index);
  result.step = index;

  if (step.action == Action::Drop) {
    Applied applied;
    applied.step = index;
    applied.action = step.action;
    applied.kind = Applied::Dropped;
    applied.before = step.commit;
    result.applied.append(applied);
    ++state.next;
    return true;
  }

  CommitPtr commit = lookupCommit(repo, step.commit);
  CommitPtr head = headCommit(repo);
  if (!commit || !head) {
    result.status = Status::Error;
    result.error = tr("Commit %1 not found.").arg(step.commit.toString());
    return false;
  }

  // Fast-forward if the commit can be reused as it is.
  bool reuse = (step.action == Action::Pick || step.action == Action::Edit);
  if (reuse && git_oid_equal(git_commit_parent_id(commit.get(), 0),
                             git_commit_id(head.get()))) {
    if (!checkoutCommit(repo, commit.get())) {
      result.status = Status::Error;
      result.error = lastError(
          tr("Unable to check out %1.").arg(step.commit.toString().left(7)));
      return false;
    }

    Applied applied;
    applied.step = index;
    applied.action = step.action;
    applied.kind = Applied::Unchanged;
    applied.before = step.commit;
    applied.after = step.commit;
    result.applied.append(applied);
    ++state.next;

    if (step.action == Action::Edit) {
      result.status = Status::Edit;
      return false;
    }

    return true;
  }

  git_cherrypick_options opts = GIT_CHERRYPICK_OPTIONS_INIT;
  opts.checkout_opts.checkout_strategy =
      GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS;
  int error = git_cherrypick(repo, commit.get(), &opts);
  removeCherryPickState(repo);
  if (error) {
    result.status = Status::Error;
    result.error = lastError(
        tr("Unable to apply %1.").arg(step.commit.toString().left(7)));
    return false;
  }

  git_index *raw = nullptr;
  if (git_repository_index(&raw, repo)) {
    result.status = Status::Error;
    result.error = lastError(tr("Unable to read the index."));
    return false;
  }

  IndexPtr repoIndex(raw, git_index_free);
  if (git_index_has_conflicts(repoIndex.get())) {
    state.conflict = true;
    result.status = Status::Conflict;
    return false;
  }

  return commitStep(state, index, QString(), result);
}

bool InteractiveRebase::commitStep(State &state, int index,
                                   const QString &message, Result &result) {
  git_repository *repo = mRepo;
  const Step step = state.steps.at(index);
  result.step = index;
  result.status = Status::Error;

  CommitPtr commit = lookupCommit(repo, step.commit);
  CommitPtr head = headCommit(repo);
  if (!commit || !head) {
    result.error = tr("Commit %1 not found.").arg(step.commit.toString());
    return false;
  }

  git_index *raw = nullptr;
  git_oid treeId;
  if (git_repository_index(&raw, repo)) {
    result.error = lastError(tr("Unable to read the index."));
    return false;
  }

  IndexPtr repoIndex(raw, git_index_free);
  if (git_index_write_tree(&treeId, repoIndex.get())) {
    result.error = lastError(tr("Unable to write the index tree."));
    return false;
  }

  git_tree *treeRaw = nullptr;
  if (git_tree_lookup(&treeRaw, repo, &treeId)) {
    result.error = lastError(tr("Unable to write the index tree."));
    return false;
  }

  TreePtr tree(treeRaw, git_tree_free);
  bool squash = (step.action == Action::Squash || step.action == Action::Fixup);

  Applied applied;
  applied.step = index;
  applied.action = step.action;
  applied.before = step.commit;

  if (!squash && !state.options.keepEmpty &&
      git_oid_equal(&treeId, git_commit_tree_id(head.get()))) {
    // Nothing left to commit (e.g. the change is already upstream).
    applied.kind = Applied::Empty;
    applied.after = Id(git_commit_id(head.get()));
  } else {
    const git_signature *author =
        git_commit_author(squash ? head.get() : commit.get());

    git_signature *committerRaw = nullptr;
    int error = 0;
    if (!state.options.committerName.isEmpty()) {
      QByteArray name = state.options.committerName.toUtf8();
      QByteArray email = state.options.committerEmail.toUtf8();
      error = state.options.committerDateIsAuthorDate
                  ? git_signature_new(&committerRaw, name.constData(),
                                      email.constData(), author->when.time,
                                      author->when.offset)
                  : git_signature_now(&committerRaw, name.constData(),
                                      email.constData());
    } else {
      git_signature *def = nullptr;
      error = git_signature_default(&def, repo);
      if (!error && state.options.committerDateIsAuthorDate) {
        error = git_signature_new(&committerRaw, def->name, def->email,
                                  author->when.time, author->when.offset);
        git_signature_free(def);
      } else {
        committerRaw = def;
      }
    }

    if (error) {
      result.error = lastError(tr("Unable to determine the committer "
                                  "signature. Check user.name and "
                                  "user.email."));
      return false;
    }

    SignaturePtr committer(committerRaw, git_signature_free);

    // Keep messages that were not edited byte for byte, like git does.
    QByteArray msg;
    if (!message.isEmpty())
      msg = prettify(message);
    else if (!step.message.isEmpty() && hasMessage(step.action))
      msg = prettify(step.message);
    else
      msg = defaultMessage(state, index).toUtf8();

    QVector<const git_commit *> parents;
    std::vector<CommitPtr> owned;
    if (squash) {
      unsigned int count = git_commit_parentcount(head.get());
      for (unsigned int i = 0; i < count; ++i) {
        git_commit *parent = nullptr;
        if (git_commit_parent(&parent, head.get(), i)) {
          result.error = lastError(tr("Unable to look up parent commit."));
          return false;
        }
        owned.emplace_back(parent, git_commit_free);
        parents.append(parent);
      }
    } else {
      parents.append(head.get());
    }

    git_oid id;
    if (git_commit_create(&id, repo, nullptr, author, committer.get(), nullptr,
                          msg.constData(), tree.get(), parents.size(),
                          parents.data()) ||
        git_repository_set_head_detached(repo, &id)) {
      result.error = lastError(tr("Unable to create commit."));
      return false;
    }

    applied.kind = Applied::Rewritten;
    applied.after = Id(id);
  }

  result.applied.append(applied);
  result.status = Status::Finished;
  state.conflict = false;
  ++state.next;

  if (step.action == Action::Edit) {
    result.status = Status::Edit;
    return false;
  }

  return true;
}

InteractiveRebase::Result InteractiveRebase::finish(State &state,
                                                    Result result) {
  git_repository *repo = mRepo;
  CommitPtr head = headCommit(repo);
  if (!head) {
    result.status = Status::Error;
    result.error = lastError(tr("Invalid HEAD."));
    return result;
  }

  if (state.headName != kDetachedHead) {
    QByteArray name = state.headName.toUtf8();
    QByteArray log = QString("rebase (interactive) (finish): %1 onto %2")
                         .arg(state.headName, state.onto.toString())
                         .toUtf8();
    git_reference *ref = nullptr;
    if (git_reference_create(&ref, repo, name.constData(),
                             git_commit_id(head.get()), 1, log.constData()) ||
        git_repository_set_head(repo, name.constData())) {
      git_reference_free(ref);
      result.status = Status::Error;
      result.error = lastError(tr("Unable to update %1.").arg(state.headName));
      return result;
    }
    git_reference_free(ref);
  }

  removeState();

  result.status = Status::Finished;
  result.step = -1;
  restoreAutostash(state, &result.error);

  emit mRepo.notifier()->referenceUpdated(mRepo.head());
  emit mRepo.notifier()->stateChanged();
  return result;
}

QString InteractiveRebase::defaultMessage(const State &state, int index) const {
  git_repository *repo = mRepo;
  const Step &step = state.steps.at(index);
  CommitPtr commit = lookupCommit(repo, step.commit);
  if (!commit)
    return QString();

  switch (step.action) {
    case Action::Reword:
      return step.message.isEmpty() ? commitMessage(commit.get())
                                    : step.message;

    case Action::Squash:
    case Action::Fixup: {
      if (!step.message.isEmpty())
        return step.message;

      CommitPtr head = headCommit(repo);
      QString previous = head ? commitMessage(head.get()) : QString();
      if (step.action == Action::Fixup)
        return previous;

      return previous.trimmed() + "\n\n" + commitMessage(commit.get());
    }

    default:
      return commitMessage(commit.get());
  }
}

bool InteractiveRebase::restoreAutostash(const State &state, QString *error) {
  if (!state.autostash.isValid())
    return true;

  QList<Commit> stashes = mRepo.stashes();
  for (int i = 0; i < stashes.size(); ++i) {
    if (stashes.at(i).id() == state.autostash) {
      if (mRepo.popStash(i))
        return true;
      break;
    }
  }

  if (error)
    *error = tr("Your autostashed changes could not be applied cleanly. "
                "They are kept in the stash list.");
  return false;
}

bool InteractiveRebase::isWorkdirDirty() const {
  git_status_options opts = GIT_STATUS_OPTIONS_INIT;
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

  git_status_list *list = nullptr;
  if (git_status_list_new(&list, mRepo, &opts))
    return true;

  size_t count = git_status_list_entrycount(list);
  git_status_list_free(list);
  return count > 0;
}

QString InteractiveRebase::statePath(const QString &file) const {
  QDir dir(QString::fromUtf8(git_repository_path(mRepo)));
  QString path = dir.filePath(kStateDir);
  return file.isEmpty() ? path : QDir(path).filePath(file);
}

bool InteractiveRebase::save(const State &state) const {
  if (!QDir().mkpath(statePath()))
    return false;

  QJsonArray steps;
  QByteArray todo;
  QByteArray done;
  for (int i = 0; i < state.steps.size(); ++i) {
    const Step &step = state.steps.at(i);
    QJsonObject obj;
    obj["action"] = actionName(step.action);
    obj["commit"] = step.commit.toString();
    obj["message"] = step.message;
    steps.append(obj);

    QByteArray line = QString("%1 %2 %3\n")
                          .arg(actionName(step.action), step.commit.toString(),
                               mRepo.lookupCommit(step.commit).summary())
                          .toUtf8();
    bool isDone = i < state.next || (i == state.next && state.conflict);
    (isDone ? done : todo).append(line);
  }

  QJsonObject options;
  options["autostash"] = state.options.autostash;
  options["keepEmpty"] = state.options.keepEmpty;
  options["committerDateIsAuthorDate"] =
      state.options.committerDateIsAuthorDate;
  options["committerName"] = state.options.committerName;
  options["committerEmail"] = state.options.committerEmail;

  QJsonObject plan;
  plan["version"] = kPlanVersion;
  plan["headName"] = state.headName;
  plan["origHead"] = state.origHead.toString();
  plan["onto"] = state.onto.toString();
  plan["autostash"] =
      state.autostash.isValid() ? state.autostash.toString() : QString();
  plan["next"] = state.next;
  plan["conflict"] = state.conflict;
  plan["options"] = options;
  plan["steps"] = steps;

  // Files used by git itself to recognize the rebase (and to abort it).
  int msgnum = state.next + (state.conflict ? 1 : 0);
  return writeFile(statePath("head-name"), state.headName.toUtf8() + "\n") &&
         writeFile(statePath("orig-head"),
                   state.origHead.toString().toUtf8() + "\n") &&
         writeFile(statePath("onto"), state.onto.toString().toUtf8() + "\n") &&
         writeFile(statePath("interactive"), QByteArray()) &&
         writeFile(statePath("end"),
                   QByteArray::number(state.steps.size()) + "\n") &&
         writeFile(statePath("msgnum"), QByteArray::number(msgnum) + "\n") &&
         writeFile(statePath("git-rebase-todo"), todo) &&
         writeFile(statePath("done"), done) &&
         writeFile(statePath(kPlanFile), QJsonDocument(plan).toJson());
}

bool InteractiveRebase::load(State &state) const {
  QFile file(statePath(kPlanFile));
  if (!file.open(QIODevice::ReadOnly))
    return false;

  QJsonObject plan = QJsonDocument::fromJson(file.readAll()).object();
  if (plan.value("version").toInt() != kPlanVersion)
    return false;

  git_oid_t type = mRepo.oidType();
  auto toId = [type](const QJsonValue &value) {
    QString hex = value.toString();
    return hex.isEmpty() ? Id() : Id(QByteArray::fromHex(hex.toUtf8()), type);
  };

  state.headName = plan.value("headName").toString();
  state.origHead = toId(plan.value("origHead"));
  state.onto = toId(plan.value("onto"));
  state.autostash = toId(plan.value("autostash"));
  state.next = plan.value("next").toInt();
  state.conflict = plan.value("conflict").toBool();

  QJsonObject options = plan.value("options").toObject();
  state.options.autostash = options.value("autostash").toBool();
  state.options.keepEmpty = options.value("keepEmpty").toBool();
  state.options.committerDateIsAuthorDate =
      options.value("committerDateIsAuthorDate").toBool();
  state.options.committerName = options.value("committerName").toString();
  state.options.committerEmail = options.value("committerEmail").toString();

  state.steps.clear();
  for (const QJsonValue &value : plan.value("steps").toArray()) {
    QJsonObject obj = value.toObject();
    Step step;
    if (!actionFromName(obj.value("action").toString(), step.action))
      return false;
    step.commit = toId(obj.value("commit"));
    step.message = obj.value("message").toString();
    state.steps.append(step);
  }

  return !state.headName.isEmpty() && state.origHead.isValid() &&
         state.next >= 0 && state.next <= state.steps.size();
}

void InteractiveRebase::removeState() const {
  QDir(statePath()).removeRecursively();
}

} // namespace git
