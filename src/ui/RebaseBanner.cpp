//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "RebaseBanner.h"
#include "app/Application.h"
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/RebaseState.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>

namespace {

// Name of a branch that points to the commit, preferring local branches.
QString branchAt(const git::Repository &repo, const git::Id &id) {
  for (git_branch_t type : {GIT_BRANCH_LOCAL, GIT_BRANCH_REMOTE}) {
    for (const git::Branch &branch : repo.branches(type)) {
      if (!branch.name().endsWith("/HEAD") && branch.target().id() == id)
        return branch.name();
    }
  }

  return QString();
}

} // namespace

RebaseBanner::RebaseBanner(const git::Repository &repo, QWidget *parent)
    : QFrame(parent), mRepo(repo) {
  setObjectName("RebaseBanner");
  Theme *theme = Application::theme();
  QColor background = theme->notice(Theme::Notice::Background);
  QColor foreground = theme->notice(Theme::Notice::Foreground);
  setStyleSheet(QString("#RebaseBanner { background-color: %1; }"
                        "#RebaseBanner QLabel { color: %2; }")
                    .arg(background.name(), foreground.name()));

  QLabel *icon = new QLabel(this);
  icon->setPixmap(
      style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(20, 20));

  mLabel = new QLabel(this);
  mLabel->setObjectName("RebaseBannerText");
  mLabel->setWordWrap(true);
  mLabel->setTextFormat(Qt::RichText);

  mContinue = new QPushButton(tr("Continue"), this);
  mContinue->setObjectName("RebaseBannerContinue");
  mContinue->setToolTip(
      tr("Continue with the next step (git rebase --continue). Resolve "
         "conflicts and stage the files first."));
  connect(mContinue, &QPushButton::clicked, this,
          &RebaseBanner::continueRequested);

  mAbort = new QPushButton(tr("Abort"), this);
  mAbort->setObjectName("RebaseBannerAbort");
  mAbort->setToolTip(tr("Stop rebasing and restore the branch to the state "
                        "before the rebase (git rebase --abort)."));
  connect(mAbort, &QPushButton::clicked, this, [this] {
    QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Abort Rebase?"),
        tr("The branch and the working tree are restored to the state before "
           "the rebase. Changes made during the rebase are lost."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes)
      emit abortRequested();
  });

  mQuit = new QPushButton(tr("Quit"), this);
  mQuit->setObjectName("RebaseBannerQuit");
  mQuit->setToolTip(tr("Stop rebasing but keep HEAD and the working tree as "
                       "they are (git rebase --quit)."));
  connect(mQuit, &QPushButton::clicked, this, &RebaseBanner::quitRequested);

  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(8);
  layout->addWidget(icon);
  layout->addWidget(mLabel, 1);
  layout->addWidget(mContinue);
  layout->addWidget(mAbort);
  layout->addWidget(mQuit);

  updateState();
}

void RebaseBanner::updateState() {
  git::RebaseState state = git::RebaseState::read(mRepo);
  if (!state.isValid()) {
    setVisible(false);
    return;
  }

  QString branch = tr("detached HEAD");
  if (!state.branch().isEmpty())
    branch = QString("<b>%1</b>").arg(state.branch().toHtmlEscaped());

  QString onto;
  if (state.onto().isValid()) {
    git::Commit commit = mRepo.lookupCommit(state.onto());
    QString id = commit.isValid() ? commit.shortId()
                                  : state.onto().toString().left(7);
    git::Reference ref = mRepo.lookupRef(state.ontoName());
    QString name = (ref.isValid() && ref.target().id() == state.onto())
                       ? ref.name()
                       : branchAt(mRepo, state.onto());
    onto = name.isEmpty()
               ? id
               : QString("<b>%1</b> (%2)").arg(name.toHtmlEscaped(), id);
  }

  QString text =
      onto.isEmpty()
          ? tr("Rebase of %1 in progress.").arg(branch)
          : tr("Rebase of %1 onto %2 in progress.").arg(branch, onto);

  if (state.total() > 0)
    text += " " + tr("Step %1 of %2.").arg(state.done()).arg(state.total());

  if (state.owner() == git::RebaseState::Owner::External)
    text += " " + tr("It was started outside of Gittyup; the buttons run "
                     "the git command line.");

  mLabel->setText(text);
  setBusy(mBusy);
  setVisible(true);
}

void RebaseBanner::setBusy(bool busy) {
  mBusy = busy;
  mContinue->setEnabled(!busy);
  mAbort->setEnabled(!busy);
  mQuit->setEnabled(!busy);
}

QString RebaseBanner::text() const { return mLabel->text(); }
