//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "InteractiveRebaseDialog.h"
#include "git/Branch.h"
#include "git/Config.h"
#include "git/Reference.h"
#include "git/Signature.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {

const int kIdRole = Qt::UserRole;
const int kActionRole = Qt::UserRole + 1;
const int kMessageRole = Qt::UserRole + 2; // custom message
const int kOriginalRole = Qt::UserRole + 3;

enum Column { ActionColumn, CommitColumn, AuthorColumn, MessageColumn };

using Action = git::InteractiveRebase::Action;

const QList<Action> kActions = {Action::Pick,   Action::Reword, Action::Edit,
                                Action::Squash, Action::Fixup,  Action::Drop};

QString actionLabel(Action action) {
  switch (action) {
    case Action::Pick:
      return InteractiveRebaseDialog::tr("Pick");
    case Action::Reword:
      return InteractiveRebaseDialog::tr("Reword");
    case Action::Edit:
      return InteractiveRebaseDialog::tr("Edit");
    case Action::Squash:
      return InteractiveRebaseDialog::tr("Squash");
    case Action::Fixup:
      return InteractiveRebaseDialog::tr("Fixup");
    case Action::Drop:
      return InteractiveRebaseDialog::tr("Drop");
  }

  return QString();
}

bool hasMessage(Action action) {
  return action == Action::Reword || action == Action::Squash;
}

QString firstLine(const QString &text) {
  return text.trimmed().section('\n', 0, 0);
}

} // namespace

InteractiveRebaseDialog::InteractiveRebaseDialog(const git::Repository &repo,
                                                 const git::Commit &base,
                                                 QWidget *parent)
    : QDialog(parent), mRepo(repo), mBase(base) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Interactive Rebase"));

  mCommits = git::InteractiveRebase::commits(repo, base, &mError);

  git::Reference head = repo.head();
  QString name = (head.isValid() && !repo.isHeadDetached())
                     ? head.name()
                     : tr("detached HEAD");
  QLabel *label =
      new QLabel(tr("Rebase %n commit(s) of <b>%1</b> onto %2 <i>%3</i>",
                    nullptr, mCommits.size())
                     .arg(name.toHtmlEscaped(), base.shortId(),
                          base.summary().toHtmlEscaped()),
                 this);

  QLabel *hint = new QLabel(
      tr("Drag commits to reorder them. Keys: P pick, R reword, E edit, "
         "S squash, F fixup, D drop, Alt+Up/Down move."),
      this);
  hint->setWordWrap(true);

  mList = new QTreeWidget(this);
  mList->setObjectName("InteractiveRebaseList");
  mList->setColumnCount(4);
  mList->setHeaderLabels(
      {tr("Action"), tr("Commit"), tr("Author"), tr("Message")});
  mList->setRootIsDecorated(false);
  mList->setUniformRowHeights(true);
  mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mList->setDragEnabled(true);
  mList->setDragDropMode(QAbstractItemView::InternalMove);
  mList->setDefaultDropAction(Qt::MoveAction);
  for (const git::Commit &commit : mCommits)
    mList->addTopLevelItem(createItem(commit));

  QHeaderView *header = mList->header();
  header->setStretchLastSection(true);
  header->setSectionResizeMode(ActionColumn, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(CommitColumn, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(AuthorColumn, QHeaderView::ResizeToContents);

  mAction = new QComboBox(this);
  mAction->setObjectName("InteractiveRebaseAction");
  for (Action action : kActions)
    mAction->addItem(actionLabel(action), static_cast<int>(action));
  connect(mAction, QOverload<int>::of(&QComboBox::activated), this,
          [this](int index) {
            setSelectedAction(
                static_cast<Action>(mAction->itemData(index).toInt()));
          });

  QPushButton *up = new QPushButton(tr("Move Up"), this);
  connect(up, &QPushButton::clicked, this, [this] { moveSelection(-1); });

  QPushButton *down = new QPushButton(tr("Move Down"), this);
  connect(down, &QPushButton::clicked, this, [this] { moveSelection(1); });

  QPushButton *autosquash = new QPushButton(tr("Autosquash"), this);
  autosquash->setToolTip(
      tr("Move 'fixup!' and 'squash!' commits behind the commits they refer "
         "to"));
  connect(autosquash, &QPushButton::clicked, this,
          &InteractiveRebaseDialog::applyAutosquash);

  QHBoxLayout *actions = new QHBoxLayout;
  actions->addWidget(new QLabel(tr("Action:"), this));
  actions->addWidget(mAction);
  actions->addStretch();
  actions->addWidget(up);
  actions->addWidget(down);
  actions->addWidget(autosquash);

  mMessageLabel = new QLabel(this);
  mMessage = new QPlainTextEdit(this);
  mMessage->setObjectName("InteractiveRebaseMessage");
  mMessage->setTabChangesFocus(true);
  connect(mMessage, &QPlainTextEdit::textChanged, this,
          &InteractiveRebaseDialog::storeMessage);

  QWidget *messageWidget = new QWidget(this);
  QVBoxLayout *messageLayout = new QVBoxLayout(messageWidget);
  messageLayout->setContentsMargins(0, 0, 0, 0);
  messageLayout->addWidget(mMessageLabel);
  messageLayout->addWidget(mMessage);

  QSplitter *splitter = new QSplitter(Qt::Vertical, this);
  splitter->setChildrenCollapsible(false);
  splitter->addWidget(mList);
  splitter->addWidget(messageWidget);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);

  mAutostash = new QCheckBox(tr("Autostash uncommitted changes"), this);
  mAutostash->setChecked(repo.gitConfig().value<bool>("rebase.autoStash"));
  mKeepEmpty = new QCheckBox(tr("Keep commits that become empty"), this);
  mCommitterDate = new QCheckBox(tr("Use author date as committer date"), this);

  QHBoxLayout *options = new QHBoxLayout;
  options->addWidget(mAutostash);
  options->addWidget(mKeepEmpty);
  options->addWidget(mCommitterDate);
  options->addStretch();

  mStatus = new QLabel(this);
  mStatus->setWordWrap(true);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  mAccept = buttons->button(QDialogButtonBox::Ok);
  mAccept->setText(tr("Start Rebase"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(label);
  layout->addWidget(hint);
  layout->addLayout(actions);
  layout->addWidget(splitter, 1);
  layout->addLayout(options);
  layout->addWidget(mStatus);
  layout->addWidget(buttons);

  // Keyboard shortcuts on the list.
  const QList<QPair<Qt::Key, Action>> keys = {
      {Qt::Key_P, Action::Pick},  {Qt::Key_R, Action::Reword},
      {Qt::Key_E, Action::Edit},  {Qt::Key_S, Action::Squash},
      {Qt::Key_F, Action::Fixup}, {Qt::Key_D, Action::Drop}};
  for (const auto &key : keys) {
    Action action = key.second;
    new QShortcut(
        QKeySequence(key.first), mList,
        [this, action] { setSelectedAction(action); }, Qt::WidgetShortcut);
  }

  new QShortcut(
      QKeySequence(Qt::ALT | Qt::Key_Up), mList, [this] { moveSelection(-1); },
      Qt::WidgetShortcut);
  new QShortcut(
      QKeySequence(Qt::ALT | Qt::Key_Down), mList, [this] { moveSelection(1); },
      Qt::WidgetShortcut);

  connect(mList, &QTreeWidget::currentItemChanged, this,
          &InteractiveRebaseDialog::updateMessageEditor);

  // Drag and drop moves rows by removing and inserting them.
  connect(
      mList->model(), &QAbstractItemModel::rowsInserted, this,
      [this] {
        updateState();
        updateMessageEditor();
      },
      Qt::QueuedConnection);

  if (!mError.isEmpty()) {
    mList->setEnabled(false);
    mAction->setEnabled(false);
    up->setEnabled(false);
    down->setEnabled(false);
    autosquash->setEnabled(false);
  } else if (mList->topLevelItemCount() > 0) {
    mList->setCurrentItem(mList->topLevelItem(0));
  }

  for (int i = 0; i < mList->topLevelItemCount(); ++i)
    updateItem(mList->topLevelItem(i));

  updateMessageEditor();
  updateState();
  resize(860, 600);
}

QList<git::InteractiveRebase::Step> InteractiveRebaseDialog::steps() const {
  QList<git::InteractiveRebase::Step> result;
  for (int i = 0; i < mList->topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = mList->topLevelItem(i);
    git::InteractiveRebase::Step step;
    step.action = action(item);
    step.commit =
        git::Id(QByteArray::fromHex(item->data(0, kIdRole).toByteArray()),
                mRepo.oidType());
    if (hasMessage(step.action))
      step.message = item->data(0, kMessageRole).toString();
    result.append(step);
  }
  return result;
}

git::InteractiveRebase::Options InteractiveRebaseDialog::options() const {
  git::InteractiveRebase::Options result;
  result.autostash = mAutostash->isChecked();
  result.keepEmpty = mKeepEmpty->isChecked();
  result.committerDateIsAuthorDate = mCommitterDate->isChecked();
  return result;
}

void InteractiveRebaseDialog::setAction(int row, Action action) {
  QTreeWidgetItem *item = mList->topLevelItem(row);
  if (!item)
    return;

  item->setData(0, kActionRole, static_cast<int>(action));
  if (!hasMessage(action))
    item->setData(0, kMessageRole, QString());

  updateItem(item);
  updateMessageEditor();
  updateState();
}

void InteractiveRebaseDialog::moveRow(int row, int delta) {
  int target = row + delta;
  if (row < 0 || target < 0 || target >= mList->topLevelItemCount())
    return;

  QTreeWidgetItem *item = mList->takeTopLevelItem(row);
  mList->insertTopLevelItem(target, item);
}

void InteractiveRebaseDialog::applyAutosquash() {
  QList<git::InteractiveRebase::Step> ordered =
      git::InteractiveRebase::autosquash(mRepo, steps());

  QMap<QString, QTreeWidgetItem *> items;
  while (mList->topLevelItemCount() > 0) {
    QTreeWidgetItem *item = mList->takeTopLevelItem(0);
    items.insert(item->data(0, kIdRole).toString(), item);
  }

  for (const git::InteractiveRebase::Step &step : ordered) {
    QTreeWidgetItem *item = items.take(step.commit.toString());
    if (!item)
      continue;

    item->setData(0, kActionRole, static_cast<int>(step.action));
    mList->addTopLevelItem(item);
    updateItem(item);
  }

  if (mList->topLevelItemCount() > 0)
    mList->setCurrentItem(mList->topLevelItem(0));

  updateMessageEditor();
  updateState();
}

QTreeWidgetItem *
InteractiveRebaseDialog::createItem(const git::Commit &commit) const {
  QTreeWidgetItem *item = new QTreeWidgetItem;
  item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                 Qt::ItemIsDragEnabled);
  item->setData(0, kIdRole, commit.id().toString());
  item->setData(0, kActionRole, static_cast<int>(Action::Pick));
  item->setData(0, kOriginalRole, commit.message());
  item->setText(CommitColumn, commit.shortId());
  item->setText(AuthorColumn, commit.author().name());
  item->setToolTip(MessageColumn, commit.message());
  return item;
}

InteractiveRebaseDialog::Action
InteractiveRebaseDialog::action(const QTreeWidgetItem *item) const {
  return static_cast<Action>(item->data(0, kActionRole).toInt());
}

QString
InteractiveRebaseDialog::originalMessage(const QTreeWidgetItem *item) const {
  return item->data(0, kOriginalRole).toString();
}

QString InteractiveRebaseDialog::effectiveMessage(int row) const {
  QTreeWidgetItem *item = mList->topLevelItem(row);
  QString custom = item->data(0, kMessageRole).toString();
  return custom.isEmpty() ? originalMessage(item) : custom;
}

QString InteractiveRebaseDialog::defaultSquashMessage(int row) const {
  // Combine the messages of the chain the squash belongs to.
  QStringList parts;
  for (int i = row; i >= 0; --i) {
    QTreeWidgetItem *item = mList->topLevelItem(i);
    Action act = action(item);
    if (act == Action::Drop || act == Action::Fixup)
      continue;

    if (act == Action::Squash) {
      QString custom = item->data(0, kMessageRole).toString();
      if (i != row && !custom.isEmpty()) {
        parts.prepend(custom.trimmed());
        break;
      }

      parts.prepend(originalMessage(item).trimmed());
      continue;
    }

    parts.prepend(effectiveMessage(i).trimmed());
    break;
  }

  return parts.join("\n\n");
}

QList<int> InteractiveRebaseDialog::selectedRows() const {
  QList<int> rows;
  for (QTreeWidgetItem *item : mList->selectedItems())
    rows.append(mList->indexOfTopLevelItem(item));
  std::sort(rows.begin(), rows.end());
  return rows;
}

void InteractiveRebaseDialog::setSelectedAction(Action action) {
  for (int row : selectedRows())
    setAction(row, action);
}

void InteractiveRebaseDialog::moveSelection(int delta) {
  QList<int> rows = selectedRows();
  if (rows.isEmpty())
    return;

  if (rows.first() + delta < 0 ||
      rows.last() + delta >= mList->topLevelItemCount())
    return;

  QTreeWidgetItem *current = mList->currentItem();
  QList<QTreeWidgetItem *> items;
  for (int row : rows)
    items.append(mList->topLevelItem(row));

  if (delta > 0)
    std::reverse(rows.begin(), rows.end());
  for (int row : rows)
    moveRow(row, delta);

  mList->clearSelection();
  for (QTreeWidgetItem *item : items)
    item->setSelected(true);
  if (current)
    mList->setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
}

void InteractiveRebaseDialog::updateItem(QTreeWidgetItem *item) {
  Action act = action(item);
  item->setText(ActionColumn, actionLabel(act));

  QString custom = item->data(0, kMessageRole).toString();
  QString summary =
      firstLine(custom.isEmpty() ? originalMessage(item) : custom);
  bool squash = (act == Action::Squash || act == Action::Fixup);
  item->setText(MessageColumn,
                squash ? QString::fromUtf8("↳ ") + summary : summary);

  QFont font = mList->font();
  font.setStrikeOut(act == Action::Drop);
  font.setItalic(squash);
  QBrush brush =
      (act == Action::Drop)
          ? mList->palette().brush(QPalette::Disabled, QPalette::Text)
          : mList->palette().brush(QPalette::Text);
  for (int column = 0; column < mList->columnCount(); ++column) {
    item->setFont(column, font);
    item->setForeground(column, brush);
  }
}

void InteractiveRebaseDialog::updateMessageEditor() {
  QTreeWidgetItem *item = mList->currentItem();
  mUpdatingMessage = true;

  if (!item) {
    mMessageLabel->setText(tr("Commit message:"));
    mMessage->clear();
    mMessage->setReadOnly(true);
    mUpdatingMessage = false;
    return;
  }

  int row = mList->indexOfTopLevelItem(item);
  Action act = action(item);
  mAction->setCurrentIndex(mAction->findData(static_cast<int>(act)));

  QString custom = item->data(0, kMessageRole).toString();
  QString text;
  switch (act) {
    case Action::Reword:
      mMessageLabel->setText(tr("Message of the reworded commit:"));
      text = custom.isEmpty() ? originalMessage(item) : custom;
      break;

    case Action::Squash:
      mMessageLabel->setText(tr("Message of the combined commit:"));
      text = custom.isEmpty() ? defaultSquashMessage(row) : custom;
      break;

    default:
      mMessageLabel->setText(
          tr("Commit message (choose Reword or Squash to change it):"));
      text = originalMessage(item);
      break;
  }

  mMessage->setReadOnly(!hasMessage(act));
  if (mMessage->toPlainText() != text)
    mMessage->setPlainText(text);

  mUpdatingMessage = false;
}

void InteractiveRebaseDialog::storeMessage() {
  if (mUpdatingMessage)
    return;

  QTreeWidgetItem *item = mList->currentItem();
  if (!item || !hasMessage(action(item)))
    return;

  int row = mList->indexOfTopLevelItem(item);
  QString text = mMessage->toPlainText();
  QString defaultText = (action(item) == Action::Reword)
                            ? originalMessage(item)
                            : defaultSquashMessage(row);

  bool isDefault = text.trimmed() == defaultText.trimmed();
  item->setData(0, kMessageRole, isDefault ? QString() : text);
  updateItem(item);
  updateState();
}

void InteractiveRebaseDialog::updateState() {
  for (int i = 0; i < mList->topLevelItemCount(); ++i)
    updateItem(mList->topLevelItem(i));

  if (!mError.isEmpty()) {
    mStatus->setText(QString("<b>%1</b>").arg(mError.toHtmlEscaped()));
    mAccept->setEnabled(false);
    return;
  }

  QList<git::InteractiveRebase::Step> list = steps();
  QString error = git::InteractiveRebase::validate(list);
  if (!error.isEmpty()) {
    mStatus->setText(QString("<b>%1</b>").arg(error.toHtmlEscaped()));
    mAccept->setEnabled(false);
    return;
  }

  bool changed = false;
  for (int i = 0; i < list.size() && !changed; ++i) {
    const git::InteractiveRebase::Step &step = list.at(i);
    changed = step.action != Action::Pick ||
              step.commit != mCommits.at(i).id() || !step.message.isEmpty();
  }

  if (!changed) {
    mStatus->setText(tr("Nothing to change yet."));
    mAccept->setEnabled(false);
    return;
  }

  QString text;
  git::Reference head = mRepo.head();
  git::Branch branch = head;
  if (branch.isValid() && !mRepo.isHeadDetached() && !mCommits.isEmpty()) {
    git::Branch upstream = branch.upstream();
    git::Commit first = mCommits.first();
    if (upstream.isValid()) {
      git::Commit tip = upstream.target();
      if (tip.isValid() && mRepo.mergeBase(tip, first).id() == first.id())
        text = tr("Some of these commits are already on %1. You will have to "
                  "force push after rebasing.")
                   .arg(upstream.name().toHtmlEscaped());
    }
  }

  mStatus->setText(text);
  mAccept->setEnabled(true);
}
