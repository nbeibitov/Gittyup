//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef CONFLICTDIALOG_H
#define CONFLICTDIALOG_H

#include "git/ConflictMarkers.h"
#include "git/Repository.h"
#include <QDialog>
#include <QList>
#include <QStringConverter>
#include <QStringList>
#include <array>

class QComboBox;
class QLabel;
class QPushButton;
class QShortcut;
class QTimer;
class QToolButton;
class TextEditor;

// Resolves the conflicts of files in three panes: ours, the editable result
// and theirs. The result is the file itself, markers included, so whatever is
// left unresolved is saved in the form git and the diff view understand.
class ConflictDialog : public QDialog {
  Q_OBJECT

public:
  ConflictDialog(const git::Repository &repo, const QStringList &files,
                 const QString &file, QWidget *parent = nullptr);

  QShortcut *nextShortcut() const { return mNextShortcut; }
  QShortcut *previousShortcut() const { return mPreviousShortcut; }

  void done(int result) override;
  void reject() override;

signals:
  void saved();

private:
  enum Pane { OursPane, ResultPane, TheirsPane, PaneCount };

  // A block as it was when the file was loaded.
  struct Origin {
    QByteArray ours;
    QByteArray theirs;
    int oursBegin; // lines, the end is exclusive
    int oursEnd;
    int theirsBegin;
    int theirsEnd;
  };

  using Anchor = std::array<int, PaneCount>;

  TextEditor *addPane(Pane pane, QLabel *label);
  void configurePane(TextEditor *editor);

  void loadFile(int index);
  void selectFile(int index);
  void refresh();
  void updateCurrent(bool force = false);
  void showBlock(int index);
  void next();
  void previous();
  void apply(int sides);
  void saveAndContinue();
  bool save();
  bool confirmDiscard();

  QByteArray resultText() const;
  void syncScroll(Pane from);
  int mapLine(Pane from, Pane to, int line);

  git::Repository mRepo;
  QStringList mFiles;
  int mFileIndex{-1};
  bool mEditable{false};
  QStringConverter::Encoding mEncoding{QStringConverter::Utf8};

  QList<Origin> mOrigins;
  QList<git::ConflictMarkers::Block> mBlocks; // of the result, as it is now
  QList<int> mMatches;                        // origin of each block, or -1
  QList<Anchor> mAnchors;
  int mCurrent{-1};
  bool mSyncing{false};

  std::array<TextEditor *, PaneCount> mPanes{};
  QLabel *mOursLabel{nullptr};
  QLabel *mTheirsLabel{nullptr};
  QComboBox *mFileBox{nullptr};
  QLabel *mStatus{nullptr};
  QToolButton *mNext{nullptr};
  QToolButton *mPrevious{nullptr};
  QPushButton *mOurs{nullptr};
  QPushButton *mTheirs{nullptr};
  QPushButton *mBoth{nullptr};
  QPushButton *mUndo{nullptr};
  QPushButton *mSave{nullptr};
  QShortcut *mNextShortcut{nullptr};
  QShortcut *mPreviousShortcut{nullptr};
  QTimer *mRefreshTimer{nullptr};
};

#endif
