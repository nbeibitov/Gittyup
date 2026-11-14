//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef BRANCHLIST_H
#define BRANCHLIST_H

#include "git/Reference.h"
#include "git/Repository.h"
#include <QTimer>
#include <QTreeWidget>

// The branches above the commit list when it shows only branches. Choosing
// one shows its history in the commit list below.
class BranchList : public QTreeWidget {
  Q_OBJECT

public:
  BranchList(const git::Repository &repo, QWidget *parent = nullptr);

  // Rebuild the rows from the branches that the filter of the commit list
  // shows (local, remote or both), the newest first.
  void refresh();

  // Mark the branch that the commit list shows without signaling.
  void setCurrent(const git::Reference &ref);

signals:
  void referenceSelected(const git::Reference &ref);

private:
  QTreeWidgetItem *findItem(const QString &name) const;

  git::Repository mRepo;
  QString mCurrent;
  QTimer mRefresh;
  bool mUpdating = false;
};

#endif
