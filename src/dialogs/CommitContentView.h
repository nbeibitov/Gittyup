//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef COMMITCONTENTVIEW_H
#define COMMITCONTENTVIEW_H

#include "git/Commit.h"
#include <QList>
#include <QWidget>

class QLabel;
class QListWidget;
class QPlainTextEdit;

// Read-only content of one commit for the rebase dialogs: author, message,
// changed files and their patch (against the first parent for merges).
class CommitContentView : public QWidget {
  Q_OBJECT

public:
  CommitContentView(QWidget *parent = nullptr);

  git::Commit commit() const { return mCommit; }
  // An invalid commit clears the view; text is shown instead.
  void setCommit(const git::Commit &commit, const QString &text = QString());

  // Used by tests.
  QStringList files() const;
  QString patch() const;

private:
  git::Commit mCommit;
  QLabel *mHeader;
  QListWidget *mFiles;
  QPlainTextEdit *mPatch;
  QList<int> mFileBlocks; // first block of each file in mPatch
};

#endif
