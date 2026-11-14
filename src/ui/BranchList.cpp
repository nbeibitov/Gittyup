//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "BranchList.h"
#include "CommitList.h"
#include "ConfigKeys.h"
#include "git/Commit.h"
#include "git/Config.h"
#include "git/Signature.h"
#include <QHeaderView>
#include <QLocale>
#include <algorithm>

namespace {

enum Column { Name, Date };

} // namespace

BranchList::BranchList(const git::Repository &repo, QWidget *parent)
    : QTreeWidget(parent), mRepo(repo) {
  setColumnCount(2);
  setHeaderHidden(true);
  setRootIsDecorated(false);
  setUniformRowHeights(true);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setFrameShape(QFrame::NoFrame);
  header()->setStretchLastSection(false);
  header()->setSectionResizeMode(Name, QHeaderView::Stretch);
  header()->setSectionResizeMode(Date, QHeaderView::ResizeToContents);

  connect(this, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *current) {
            if (mUpdating || !current)
              return;

            git::Reference ref =
                current->data(Name, Qt::UserRole).value<git::Reference>();
            mCurrent = ref.isValid() ? ref.name() : QString();
            if (ref.isValid())
              emit referenceSelected(ref);
          });

  // Branches change in bursts, e.g. on fetch.
  mRefresh.setSingleShot(true);
  mRefresh.setInterval(0);
  connect(&mRefresh, &QTimer::timeout, this, &BranchList::refresh);

  git::RepositoryNotifier *notifier = repo.notifier();
  auto schedule = [this] {
    if (!isHidden())
      mRefresh.start();
  };
  connect(notifier, &git::RepositoryNotifier::referenceAdded, this, schedule);
  connect(notifier, &git::RepositoryNotifier::referenceRemoved, this, schedule);
  connect(notifier, &git::RepositoryNotifier::referenceUpdated, this,
          schedule);
}

void BranchList::refresh() {
  int filter = mRepo.appConfig().value<int>(
      ConfigKeys::kRefsKey, (int)CommitList::RefsFilter::AllRefs);
  bool local = (filter != (int)CommitList::RefsFilter::RemoteRefs);
  bool remote = (filter != (int)CommitList::RefsFilter::LocalRefs);

  struct Entry {
    git::Reference ref;
    git::Commit commit;
  };

  QList<Entry> entries;
  for (const git::Reference &ref : mRepo.refs()) {
    if (!(local && ref.isLocalBranch()) && !(remote && ref.isRemoteBranch()))
      continue;

    // The symbolic HEAD of a remote is no branch of its own.
    if (ref.isRemoteBranch() && ref.name().endsWith("/HEAD"))
      continue;

    git::Commit commit = ref.target();
    if (commit.isValid())
      entries.append({ref, commit});
  }

  // The newest first.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry &lhs, const Entry &rhs) {
                     return lhs.commit.committer().date() >
                            rhs.commit.committer().date();
                   });

  mUpdating = true;
  clear();
  QLocale locale;
  for (const Entry &entry : entries) {
    QTreeWidgetItem *item = new QTreeWidgetItem(this);
    item->setText(Name, entry.ref.name());
    item->setData(Name, Qt::UserRole, QVariant::fromValue(entry.ref));
    item->setText(Date,
                  locale.toString(entry.commit.committer().date().toLocalTime(),
                                  QLocale::ShortFormat));
    item->setToolTip(Name, entry.commit.summary());
    item->setToolTip(Date, entry.commit.summary());
    if (entry.ref.isHead()) {
      QFont font = item->font(Name);
      font.setBold(true);
      item->setFont(Name, font);
    }
  }

  if (QTreeWidgetItem *item = findItem(mCurrent))
    setCurrentItem(item);
  mUpdating = false;
}

void BranchList::setCurrent(const git::Reference &ref) {
  mCurrent = ref.isValid() ? ref.name() : QString();
  mUpdating = true;
  if (QTreeWidgetItem *item = findItem(mCurrent)) {
    setCurrentItem(item);
  } else {
    clearSelection();
    setCurrentItem(nullptr);
  }
  mUpdating = false;
}

QTreeWidgetItem *BranchList::findItem(const QString &name) const {
  if (name.isEmpty())
    return nullptr;

  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = topLevelItem(i);
    if (item->text(Name) == name)
      return item;
  }

  return nullptr;
}
