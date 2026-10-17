//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef REBASETODODIALOG_H
#define REBASETODODIALOG_H

#include <QDialog>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

// Edits the todo list that 'git rebase -i' writes, including the label,
// reset and merge commands of --rebase-merges.
class RebaseTodoDialog : public QDialog {
  Q_OBJECT

public:
  RebaseTodoDialog(const QString &todo, QWidget *parent = nullptr);

  // The edited todo list.
  QString todo() const;

  // Row manipulation (also used by tests).
  int rowCount() const;
  QString command(int row) const;
  // The actions that apply to the row: all commit actions for commits;
  // pick, reword and drop for merges; none for label, reset, ...
  QStringList commands(int row) const;
  // One of commands(row). Returns false if it doesn't apply.
  bool setCommand(int row, const QString &command);
  void moveRow(int row, int delta);

  // Returns an error message if git would reject the list.
  QString validate() const;

private:
  void setRows(const QString &todo);
  QString rowsText() const;
  QTreeWidgetItem *createItem(const QString &line) const;
  void updateItem(QTreeWidgetItem *item) const;
  QList<int> selectedRows() const;
  void setSelectedCommand(const QString &command);
  void moveSelection(int delta);
  void updateAction();
  void updateState();

  QTabWidget *mTabs;
  QTreeWidget *mList;
  QComboBox *mAction;
  QPlainTextEdit *mText;
  QLabel *mStatus;
  QPushButton *mAccept;
};

#endif
