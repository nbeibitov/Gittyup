//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "RebaseTodoDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {

const int kCommandRole = Qt::UserRole;
const int kOptionRole = Qt::UserRole + 1;
const int kArgsRole = Qt::UserRole + 2;
const int kDroppedRole = Qt::UserRole + 3; // merges

// Actions that apply to merges.
const QStringList kMergeCommands = {"pick", "reword", "drop"};

enum Column { ActionColumn, CommitColumn, DescriptionColumn };

const QStringList kCommitCommands = {"pick",  "reword", "edit",
                                     "squash", "fixup", "drop"};

QString fullCommand(const QString &command) {
  static const QMap<QString, QString> abbreviations = {
      {"p", "pick"},  {"r", "reword"}, {"e", "edit"},  {"s", "squash"},
      {"f", "fixup"}, {"d", "drop"},   {"x", "exec"},  {"b", "break"},
      {"l", "label"}, {"t", "reset"},  {"m", "merge"}, {"u", "update-ref"}};
  return abbreviations.value(command, command);
}

QString commandLabel(const QString &command, const QString &option) {
  if (command == "pick")
    return RebaseTodoDialog::tr("Pick");
  if (command == "reword")
    return RebaseTodoDialog::tr("Reword");
  if (command == "edit")
    return RebaseTodoDialog::tr("Edit");
  if (command == "squash")
    return RebaseTodoDialog::tr("Squash");
  if (command == "fixup")
    return RebaseTodoDialog::tr("Fixup");
  if (command == "drop")
    return RebaseTodoDialog::tr("Drop");
  if (command == "merge")
    return (option == "-c") ? RebaseTodoDialog::tr("Merge (reword)")
                            : RebaseTodoDialog::tr("Merge");
  if (command == "label")
    return RebaseTodoDialog::tr("Label");
  if (command == "reset")
    return RebaseTodoDialog::tr("Reset to");
  return command;
}

bool isCommit(const QString &command) {
  return kCommitCommands.contains(command);
}

// Commands that don't change HEAD.
bool isTransparent(const QString &command) {
  return command == "drop" || command == "label" || command == "exec" ||
         command == "break" || command == "update-ref" || command == "noop";
}

} // namespace

RebaseTodoDialog::RebaseTodoDialog(const QString &todo, QWidget *parent)
    : QDialog(parent) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Interactive Rebase"));
  resize(760, 520);

  QLabel *hint = new QLabel(
      tr("Git asks how to replay the commits. Drag rows to reorder them. "
         "Keys: P pick, R reword, E edit, S squash, F fixup, D drop, "
         "Alt+Up/Down move. Merges can be kept, reworded or dropped. The "
         "gray Label and Reset rows are bookmarks git needs to recreate the "
         "merges: 'Reset to' starts a branch at a bookmark, 'Label' "
         "remembers its end for the merge."),
      this);
  hint->setWordWrap(true);

  mList = new QTreeWidget(this);
  mList->setObjectName("RebaseTodoList");
  mList->setColumnCount(3);
  mList->setHeaderLabels({tr("Action"), tr("Commit"), tr("Description")});
  mList->setRootIsDecorated(false);
  mList->setUniformRowHeights(true);
  mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mList->setDragEnabled(true);
  mList->setDragDropMode(QAbstractItemView::InternalMove);
  mList->setDefaultDropAction(Qt::MoveAction);

  QHeaderView *header = mList->header();
  header->setStretchLastSection(true);
  header->setSectionResizeMode(ActionColumn, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(CommitColumn, QHeaderView::ResizeToContents);

  mAction = new QComboBox(this);
  mAction->setObjectName("RebaseTodoAction");
  for (const QString &command : kCommitCommands)
    mAction->addItem(commandLabel(command, QString()), command);
  connect(mAction, QOverload<int>::of(&QComboBox::activated), this,
          [this](int index) {
            setSelectedCommand(mAction->itemData(index).toString());
          });

  QPushButton *up = new QPushButton(tr("Move Up"), this);
  connect(up, &QPushButton::clicked, this, [this] { moveSelection(-1); });

  QPushButton *down = new QPushButton(tr("Move Down"), this);
  connect(down, &QPushButton::clicked, this, [this] { moveSelection(1); });

  QHBoxLayout *actions = new QHBoxLayout;
  actions->addWidget(new QLabel(tr("Action:"), this));
  actions->addWidget(mAction);
  actions->addStretch();
  actions->addWidget(up);
  actions->addWidget(down);

  QWidget *steps = new QWidget(this);
  QVBoxLayout *stepsLayout = new QVBoxLayout(steps);
  stepsLayout->addLayout(actions);
  stepsLayout->addWidget(mList, 1);

  mText = new QPlainTextEdit(this);
  mText->setObjectName("RebaseTodoText");
  mText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  mText->setLineWrapMode(QPlainTextEdit::NoWrap);

  mTabs = new QTabWidget(this);
  mTabs->addTab(steps, tr("Steps"));
  mTabs->addTab(mText, tr("Text"));
  connect(mTabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 1) {
      mText->setPlainText(rowsText());
    } else {
      setRows(mText->toPlainText());
    }
    updateState();
  });
  connect(mText, &QPlainTextEdit::textChanged, this,
          &RebaseTodoDialog::updateState);

  mStatus = new QLabel(this);
  mStatus->setWordWrap(true);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  mAccept = buttons->button(QDialogButtonBox::Ok);
  mAccept->setText(tr("Start Rebase"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel Rebase"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(hint);
  layout->addWidget(mTabs, 1);
  layout->addWidget(mStatus);
  layout->addWidget(buttons);

  const QList<QPair<Qt::Key, QString>> keys = {
      {Qt::Key_P, "pick"},   {Qt::Key_R, "reword"}, {Qt::Key_E, "edit"},
      {Qt::Key_S, "squash"}, {Qt::Key_F, "fixup"},  {Qt::Key_D, "drop"}};
  for (const auto &key : keys) {
    QString command = key.second;
    new QShortcut(
        QKeySequence(key.first), mList,
        [this, command] { setSelectedCommand(command); }, Qt::WidgetShortcut);
  }

  new QShortcut(
      QKeySequence(Qt::ALT | Qt::Key_Up), mList, [this] { moveSelection(-1); },
      Qt::WidgetShortcut);
  new QShortcut(
      QKeySequence(Qt::ALT | Qt::Key_Down), mList, [this] { moveSelection(1); },
      Qt::WidgetShortcut);

  connect(mList, &QTreeWidget::currentItemChanged, this,
          &RebaseTodoDialog::updateAction);

  // Drag and drop moves rows by removing and inserting them.
  connect(mList->model(), &QAbstractItemModel::rowsInserted, this,
          &RebaseTodoDialog::updateState, Qt::QueuedConnection);

  setRows(todo);
  if (mList->topLevelItemCount())
    mList->setCurrentItem(mList->topLevelItem(0));
  updateState();
}

QString RebaseTodoDialog::todo() const {
  return (mTabs->currentIndex() == 1) ? mText->toPlainText() : rowsText();
}

int RebaseTodoDialog::rowCount() const { return mList->topLevelItemCount(); }

QString RebaseTodoDialog::command(int row) const {
  QTreeWidgetItem *item = mList->topLevelItem(row);
  if (!item)
    return QString();

  QString command = item->data(0, kCommandRole).toString();
  if (command == "merge") {
    if (item->data(0, kDroppedRole).toBool())
      return "drop";
    if (item->data(0, kOptionRole).toString() == "-c")
      return "reword";
  }
  return command;
}

QStringList RebaseTodoDialog::commands(int row) const {
  QTreeWidgetItem *item = mList->topLevelItem(row);
  if (!item)
    return QStringList();

  QString command = item->data(0, kCommandRole).toString();
  if (isCommit(command))
    return kCommitCommands;
  if (command != "merge")
    return QStringList();

  // Only merges that recreate a commit have a message to reword.
  if (item->data(0, kOptionRole).toString().isEmpty())
    return {"pick", "drop"};
  return kMergeCommands;
}

bool RebaseTodoDialog::setCommand(int row, const QString &command) {
  QTreeWidgetItem *item = mList->topLevelItem(row);
  if (!item)
    return false;

  QString target = (command == "merge") ? QString("pick") : command;
  if (!commands(row).contains(target))
    return false;

  QString current = item->data(0, kCommandRole).toString();
  if (current == "merge") {
    item->setData(0, kDroppedRole, target == "drop");
    if (target != "drop" && !item->data(0, kOptionRole).toString().isEmpty())
      item->setData(0, kOptionRole, target == "reword" ? "-c" : "-C");
  } else if (isCommit(current) && isCommit(command)) {
    item->setData(0, kCommandRole, command);
    item->setData(0, kOptionRole, QString()); // fixup -C/-c
  } else {
    return false;
  }

  updateItem(item);
  updateState();
  if (item == mList->currentItem())
    updateAction();
  return true;
}

void RebaseTodoDialog::moveRow(int row, int delta) {
  int target = row + delta;
  if (row < 0 || row >= rowCount() || target < 0 || target >= rowCount())
    return;

  QTreeWidgetItem *item = mList->takeTopLevelItem(row);
  mList->insertTopLevelItem(target, item);
  updateState();
}

QString RebaseTodoDialog::validate() const {
  bool hasCommit = false;
  for (int row = 0; row < rowCount(); ++row) {
    QString command = this->command(row);
    if (command == "squash" || command == "fixup") {
      if (!hasCommit)
        return tr("'%1' in row %2 has no previous commit to combine with.")
            .arg(commandLabel(command, QString()))
            .arg(row + 1);
    }

    if (command == "reset") {
      hasCommit = false;
    } else if (!isTransparent(command)) {
      hasCommit = true;
    }
  }

  return QString();
}

void RebaseTodoDialog::setRows(const QString &todo) {
  mList->clear();
  for (const QString &line : todo.split('\n')) {
    QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#'))
      continue;
    mList->addTopLevelItem(createItem(trimmed));
  }
}

QString RebaseTodoDialog::rowsText() const {
  QStringList lines;
  for (int row = 0; row < rowCount(); ++row) {
    QTreeWidgetItem *item = mList->topLevelItem(row);
    if (item->data(0, kDroppedRole).toBool())
      continue; // dropped merge
    QStringList parts = {item->data(0, kCommandRole).toString()};
    QString option = item->data(0, kOptionRole).toString();
    if (!option.isEmpty())
      parts.append(option);
    QString args = item->data(0, kArgsRole).toString();
    if (!args.isEmpty())
      parts.append(args);
    lines.append(parts.join(' '));
  }

  return lines.join('\n') + '\n';
}

QTreeWidgetItem *RebaseTodoDialog::createItem(const QString &line) const {
  QString command = fullCommand(line.section(' ', 0, 0, QString::SectionSkipEmpty));
  QString args = line.section(' ', 1, -1, QString::SectionSkipEmpty);

  // fixup -C/-c <commit> and merge -C/-c <commit> <label>
  QString option;
  if ((command == "fixup" || command == "merge") &&
      (args.startsWith("-C ") || args.startsWith("-c "))) {
    option = args.left(2);
    args = args.mid(3).trimmed();
  }

  QTreeWidgetItem *item = new QTreeWidgetItem;
  item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                 Qt::ItemIsDragEnabled);
  item->setData(0, kCommandRole, command);
  item->setData(0, kOptionRole, option);
  item->setData(0, kArgsRole, args);
  updateItem(item);
  return item;
}

void RebaseTodoDialog::updateItem(QTreeWidgetItem *item) const {
  QString command = item->data(0, kCommandRole).toString();
  QString option = item->data(0, kOptionRole).toString();
  QString args = item->data(0, kArgsRole).toString();

  QString commit;
  QString description = args;
  if (isCommit(command) || (command == "merge" && !option.isEmpty())) {
    commit = args.section(' ', 0, 0);
    description = args.section(' ', 1);
    if (description.startsWith("# "))
      description = description.mid(2);
  }

  item->setText(ActionColumn, commandLabel(command, option));
  item->setText(CommitColumn, commit);
  item->setText(DescriptionColumn, description);

  bool structural = !isCommit(command) && command != "merge";
  bool dropped = (command == "drop" || item->data(0, kDroppedRole).toBool());
  for (int column = 0; column < 3; ++column) {
    QFont font = item->font(column);
    font.setItalic(structural);
    font.setStrikeOut(dropped);
    item->setFont(column, font);
    item->setForeground(column, structural || dropped
                                    ? mList->palette().brush(
                                          QPalette::Disabled, QPalette::Text)
                                    : mList->palette().brush(QPalette::Text));
  }
}

QList<int> RebaseTodoDialog::selectedRows() const {
  QList<int> rows;
  for (QTreeWidgetItem *item : mList->selectedItems())
    rows.append(mList->indexOfTopLevelItem(item));
  std::sort(rows.begin(), rows.end());
  return rows;
}

void RebaseTodoDialog::setSelectedCommand(const QString &command) {
  bool skipped = false;
  for (int row : selectedRows()) {
    if (!setCommand(row, command))
      skipped = true;
  }

  // Say why nothing happened.
  if (skipped && mStatus->text().isEmpty())
    mStatus->setText(
        tr("'%1' doesn't apply to some of the selected rows. Merges can only "
           "be kept, reworded or dropped; Label and Reset rows can be "
           "changed in the Text tab.")
            .arg(commandLabel(command, QString())));
}

void RebaseTodoDialog::updateAction() {
  QTreeWidgetItem *item = mList->currentItem();
  int row = item ? mList->indexOfTopLevelItem(item) : -1;
  QStringList commands = this->commands(row);

  // Offer only the actions that apply to the current row.
  QStandardItemModel *model = qobject_cast<QStandardItemModel *>(mAction->model());
  for (int i = 0; i < mAction->count(); ++i) {
    bool enabled = commands.contains(mAction->itemData(i).toString());
    if (model)
      model->item(i)->setEnabled(enabled);
  }

  mAction->setEnabled(!commands.isEmpty());
  QString command = this->command(row);
  int index = mAction->findData(command == "merge" ? QString("pick") : command);
  if (index >= 0)
    mAction->setCurrentIndex(index);
}

void RebaseTodoDialog::moveSelection(int delta) {
  QList<int> rows = selectedRows();
  if (rows.isEmpty())
    return;
  if ((delta < 0 && rows.first() == 0) ||
      (delta > 0 && rows.last() == rowCount() - 1))
    return;

  if (delta > 0)
    std::reverse(rows.begin(), rows.end());

  QList<QTreeWidgetItem *> items;
  for (int row : rows) {
    items.append(mList->topLevelItem(row));
    moveRow(row, delta);
  }

  mList->clearSelection();
  for (QTreeWidgetItem *item : items)
    item->setSelected(true);
  mList->setCurrentItem(items.first(), 0, QItemSelectionModel::NoUpdate);
}

void RebaseTodoDialog::updateState() {
  QString error;
  if (mTabs->currentIndex() == 0)
    error = validate();

  mStatus->setText(error);
  mAccept->setEnabled(error.isEmpty());
}
