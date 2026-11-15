//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef COMMITLIST_H
#define COMMITLIST_H

#include "git/Reference.h"
#include "git/Id.h"
#include <QListView>
#include <QHash>
#include <QSet>

class QDateTime;
#include <QTimer>

class Index;

namespace git {
class Commit;
class Diff;
} // namespace git

class CommitList : public QListView {
  Q_OBJECT

public:
  enum Role { DiffRole = Qt::UserRole, CommitRole, GraphRole, GraphColorRole };
  enum class RefsFilter {
    AllRefs,
    SelectedRef,
    SelectedRefIgnoreMerge,
    // All references of one kind. Local includes tags.
    LocalRefs,
    RemoteRefs,
  };

  // All references (or all of one kind) instead of the selected one.
  static bool showsAllRefs(RefsFilter filter);

  CommitList(Index *index, QWidget *parent = nullptr);

  // Get the status diff item.
  git::Diff status() const;

  // Get the current selection.
  QString selectedRange() const;
  git::Diff selectedDiff() const;
  QList<git::Commit> selectedCommits() const;

  // Cancel background status diff.
  void cancelStatus();

  void setReference(const git::Reference &ref);
  void setFilter(const QString &filter);
  void setPathspec(const QString &pathspec, bool index = false);
  void setCommits(const QList<git::Commit> &commits);

  void selectReference(const git::Reference &ref);
  void resetSelection(bool spontaneous = false);
  void selectFirstCommit(bool spontaneous = false);
  void selectCommitRelative(int offset);

  // Follow a line of the history from the selected row. The uncommitted
  // changes are a child of HEAD. Return false if there is no such commit in
  // the list, e.g. because the list is filtered.
  bool selectParent(int number = 0);

  // Where the history branches off, go back to the child that the last
  // move to a parent came from, or else let the user choose; choose always
  // asks. The child on the current branch is offered first.
  bool selectChild(bool choose = false);

  // The children of the commit among the rows above it, where a
  // topological order puts them. An invalid commit in the result stands for
  // the uncommitted changes.
  QList<git::Commit> childCommits(const git::Commit &commit);

  // Select the best common ancestor of the commits, where their lines of
  // the history meet. Returns false if there is none in the list.
  bool selectMergeBase(const git::Commit &lhs, const git::Commit &rhs);

  // Whether the commit is on the line of first parents through the single
  // selected commit, among the loaded rows, which the graph highlights.
  bool isOnSelectedLine(const git::Commit &commit) const;

  // Select the next older or newer merge or tagged commit from the
  // selection, loading more rows for older ones. Return false if there is
  // none.
  enum class Landmark { Merge, Tag };
  bool selectLandmark(Landmark landmark, bool older);

  // Select the newest commit that was committed at or before the date.
  bool selectDate(const QDateTime &date);

  // In this mode the arrow keys move only along the branch of the list:
  // the line of first parents from the tip of the selected reference. It
  // applies to all lists and is remembered.
  static bool branchNavigation();
  static void setBranchNavigation(bool enabled);
  bool selectRange(const QString &range, const QString &file = QString(),
                   bool spontaneous = false);
  void suppressResetWalker(bool suppress);
  bool isResetWalkerSuppressed();

  void resetSettings();
  void resetReference(const git::Reference &ref);

  void setModel(QAbstractItemModel *model) override;

  // Whether a status check and/or walker/row rebuild is currently in
  // flight. See the loadingChanged() signal for a way to wait on this
  // instead of polling it.
  bool isLoading() const { return mLoading || mFetching; }

signals:
  void statusChanged(bool dirty);
  void diffSelected(const git::Diff diff, const QString &file = QString(),
                    bool spontaneous = false);

  // Emitted just before a (potentially slow) diff is being computation. This
  // can be used to clear GUI and enable loading indicators whilst waiting
  void diffLoading();

  // Emitted whenever isLoading() changes.
  void loadingChanged(bool loading);

  // Progress of the running status check (about once a second), and its
  // result. Files counts the examined files, total the tracked files.
  void statusProgress(int files, int total, qint64 msecs);
  void statusChecked(int changes, int files, qint64 msecs);

protected:
  void contextMenuEvent(QContextMenuEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *) override;
  void paintEvent(QPaintEvent *event) override;
  QModelIndex moveCursor(CursorAction action,
                         Qt::KeyboardModifiers modifiers) override;

private:
  // The next row on the branch below or above the row, the row itself if
  // there is none, or an invalid index if the mode doesn't apply.
  QModelIndex branchRow(int row, bool down);

  // Whether the loaded commit is on the line of first parents from the tip
  // of the selected reference. The uncommitted changes are on HEAD's.
  bool isOnBranch(const git::Commit &commit);

  // The child that a move to the parent came from, by parent.
  QHash<git::Id, git::Id> mCameFrom;

  void storeSelection();
  void restoreSelection();
  void updateModel();
  void setLoading(bool loading);
  void setFetching(bool fetching);
  void updateLoading();

  QModelIndexList sortedIndexes() const;

  QModelIndex findCommit(const git::Commit &commit);
  void selectIndexes(const QItemSelection &selection,
                     const QString &file = QString(), bool spontaneous = false);

  void notifySelectionChanged();
  void dispatchSelectedDiff(const QString &file, bool spontaneous);

  bool isDecoration(const QModelIndex &index, const QPoint &pos);
  bool isStar(const QModelIndex &index, const QPoint &pos);

  QString mFile;
  QSet<git::Id> mLine; // see isOnSelectedLine()
  QModelIndex mStar;
  QModelIndex mCancel;
  bool mSpontaneous = true;

  Index *mIndex;
  QString mFilter;

  // The path that the search index looks up, within the shown history.
  QString mPathFilter;

  QAbstractListModel *mList;
  QAbstractListModel *mModel;

  bool mRestoreSelection{true};

  QString mSelectedRange;

  // Whether the current selection is just the automatic fallback rather
  // than a deliberate user pick
  bool mSelectionIsDefault{false};

  // Whether the loading indicator should be shown
  bool mLoading{false};
  bool mFetching{false}; // more rows are loaded in the background
  bool mReportedLoading{false};
  float mLoadingFadein = 0;
  int mProgress{0};
  QTimer mTimer;

  // Incremented on every selection-driven diff request. This is a hack used to
  // discard diffs that arrive before the last one
  // (Yes, we should have a proper cancel pathway here)
  int mDiffRequest = 0;
};

#endif
