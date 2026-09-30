//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef REBASEBANNER_H
#define REBASEBANNER_H

#include "git/Repository.h"
#include <QFrame>

class QLabel;
class QPushButton;

// Notice shown while a rebase is in progress, with buttons to continue,
// abort or quit it. Rebases started outside of Gittyup are included.
class RebaseBanner : public QFrame {
  Q_OBJECT

public:
  RebaseBanner(const git::Repository &repo, QWidget *parent = nullptr);

  // Re-read the rebase state and show or hide the banner.
  void updateState();

  // Disable the buttons while a command runs.
  void setBusy(bool busy);

  QString text() const;

signals:
  void continueRequested();
  void abortRequested();
  void quitRequested();

private:
  git::Repository mRepo;
  QLabel *mLabel;
  QPushButton *mContinue;
  QPushButton *mAbort;
  QPushButton *mQuit;
  bool mBusy = false;
};

#endif
