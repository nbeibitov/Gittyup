//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef INTERACTIVEREBASEDIALOG_H
#define INTERACTIVEREBASEDIALOG_H

#include "git/Commit.h"
#include "git/InteractiveRebase.h"
#include "git/Reference.h"
#include "git/Repository.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class InteractiveRebaseDialog : public QDialog {
  Q_OBJECT

public:
  using Action = git::InteractiveRebase::Action;

  // Rebase the commits after base (exclusive) up to HEAD.
  InteractiveRebaseDialog(const git::Repository &repo, const git::Commit &base,
                          QWidget *parent = nullptr);

  // Error that prevents an interactive rebase (e.g. merge commits in range).
  QString error() const { return mError; }

  git::Commit base() const { return mBase; }
  QList<git::InteractiveRebase::Step> steps() const;
  git::InteractiveRebase::Options options() const;

  // Row manipulation (also used by tests).
  void setAction(int row, Action action);
  void moveRow(int row, int delta);
  void applyAutosquash();

  // Extend the range by the parent of the base commit.
  void includeOlderCommit();

  // Rebase onto the target of ref instead of rewriting the commits in place
  // (invalid ref). The range is extended back to the merge base with ref.
  void setOnto(const git::Reference &ref);

private:
  QTreeWidgetItem *createItem(const git::Commit &commit) const;
  Action action(const QTreeWidgetItem *item) const;
  QString originalMessage(const QTreeWidgetItem *item) const;
  QString effectiveMessage(int row) const;
  QString defaultSquashMessage(int row) const;
  QList<int> selectedRows() const;

  void setSelectedAction(Action action);
  void moveSelection(int delta);
  void updateItem(QTreeWidgetItem *item);
  void updateMessageEditor();
  void storeMessage();
  void updateState();
  void updateRange();
  bool prependBase();

  git::Repository mRepo;
  git::Commit mBase;
  git::Reference mOntoRef;
  QList<git::Commit> mCommits;
  QString mError;
  bool mUpdatingMessage = false;

  QLabel *mRange;
  QComboBox *mOnto;
  QPushButton *mOlder;
  QTreeWidget *mList;
  QComboBox *mAction;
  QLabel *mMessageLabel;
  QPlainTextEdit *mMessage;
  QCheckBox *mAutostash;
  QCheckBox *mKeepEmpty;
  QCheckBox *mCommitterDate;
  QLabel *mStatus;
  QPushButton *mAccept;
};

#endif
