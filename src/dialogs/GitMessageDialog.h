//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef GITMESSAGEDIALOG_H
#define GITMESSAGEDIALOG_H

#include <QDialog>

class QPlainTextEdit;
class QPushButton;

// Edits a commit message that git asks for, e.g. when a rebase rewords or
// squashes commits. Git's comment lines are shown separately and kept.
class GitMessageDialog : public QDialog {
  Q_OBJECT

public:
  GitMessageDialog(const QString &text, QChar commentChar,
                   QWidget *parent = nullptr);

  // The message with git's comments appended.
  QString text() const;

  QString message() const;
  void setMessage(const QString &message);

private:
  QChar mCommentChar;
  QString mComments;
  QPlainTextEdit *mMessage;
  QPushButton *mAccept;
};

#endif
