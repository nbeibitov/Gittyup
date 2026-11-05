//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "ConflictDialog.h"
#include "app/Application.h"
#include "app/Theme.h"
#include "editor/TextEditor.h"
#include "git/Index.h"
#include "git/Patch.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using Markers = git::ConflictMarkers;

namespace {

const QString kGeometryKey = "conflictdialog/geometry";
const int kContextLines = 3;
const int kCurrentBlockMargin = 6;

int colour(const QColor &color) {
  return color.red() | (color.green() << 8) | (color.blue() << 16);
}

void addMarker(TextEditor *editor, int marker, int begin, int end) {
  for (int line = begin; line < end; ++line)
    editor->markerAdd(line, marker);
}

QString decodedWith(QStringConverter::Encoding encoding,
                    const QByteArray &bytes) {
  // Keeps a byte order mark as a character, to write it back.
  QStringDecoder decoder(encoding, QStringDecoder::Flag::ConvertInitialBom);
  return decoder.decode(bytes);
}

QByteArray encodedWith(QStringConverter::Encoding encoding,
                       const QString &text) {
  QStringEncoder encoder(encoding);
  return encoder.encode(text);
}

// Text that doesn't survive the way back would be damaged by saving it.
bool survives(QStringConverter::Encoding encoding, const QString &text,
              const QByteArray &bytes) {
  return !text.contains(QChar::Null) && encodedWith(encoding, text) == bytes;
}

} // namespace

ConflictDialog::ConflictDialog(const git::Repository &repo,
                               const QStringList &files, const QString &file,
                               QWidget *parent)
    : QDialog(parent), mRepo(repo), mFiles(files) {
  setWindowTitle(tr("Resolve Conflicts"));
  setWindowFlag(Qt::WindowMaximizeButtonHint);

  mFileBox = new QComboBox(this);
  mFileBox->setObjectName("ConflictFile");
  mFileBox->addItems(mFiles);
  connect(mFileBox, &QComboBox::activated, this, &ConflictDialog::selectFile);

  mStatus = new QLabel(this);
  mStatus->setObjectName("ConflictStatus");

  mPrevious = new QToolButton(this);
  mPrevious->setObjectName("ConflictPrevious");
  mPrevious->setText(tr("Previous"));
  mPrevious->setToolTip(tr("Go to the previous conflict"));
  connect(mPrevious, &QToolButton::clicked, this, &ConflictDialog::previous);

  mNext = new QToolButton(this);
  mNext->setObjectName("ConflictNext");
  mNext->setText(tr("Next"));
  mNext->setToolTip(tr("Go to the next conflict"));
  connect(mNext, &QToolButton::clicked, this, &ConflictDialog::next);

  mNextShortcut = new QShortcut(this);
  connect(mNextShortcut, &QShortcut::activated, mNext, &QToolButton::click);
  mPreviousShortcut = new QShortcut(this);
  connect(mPreviousShortcut, &QShortcut::activated, mPrevious,
          &QToolButton::click);

  QHBoxLayout *top = new QHBoxLayout;
  top->addWidget(mFileBox, 1);
  top->addWidget(mStatus, 1);
  top->addWidget(mPrevious);
  top->addWidget(mNext);

  mOursLabel = new QLabel(this);
  mOursLabel->setObjectName("ConflictOursLabel");
  mTheirsLabel = new QLabel(this);
  mTheirsLabel->setObjectName("ConflictTheirsLabel");
  QLabel *resultLabel = new QLabel(tr("Result (editable)"), this);

  QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);
  struct {
    Pane pane;
    QLabel *label;
    const char *name;
  } panes[] = {{OursPane, mOursLabel, "ConflictOursPane"},
               {ResultPane, resultLabel, "ConflictResultPane"},
               {TheirsPane, mTheirsLabel, "ConflictTheirsPane"}};
  for (const auto &item : panes) {
    TextEditor *editor = addPane(item.pane, item.label);
    editor->setObjectName(item.name);

    QWidget *column = new QWidget(splitter);
    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(item.label);
    layout->addWidget(editor);
    splitter->addWidget(column);
    splitter->setStretchFactor(item.pane, 1);
  }
  splitter->setSizes({1000, 1000, 1000});

  mOurs = new QPushButton(tr("Use Ours"), this);
  mOurs->setObjectName("ConflictOurs");
  mOurs->setStyleSheet(
      Application::theme()->diffButtonStyle(Theme::Diff::Ours));
  connect(mOurs, &QPushButton::clicked, this, [this] { apply(Markers::Ours); });

  mTheirs = new QPushButton(tr("Use Theirs"), this);
  mTheirs->setObjectName("ConflictTheirs");
  mTheirs->setStyleSheet(
      Application::theme()->diffButtonStyle(Theme::Diff::Theirs));
  connect(mTheirs, &QPushButton::clicked, this,
          [this] { apply(Markers::Theirs); });

  mBoth = new QPushButton(tr("Use Both"), this);
  mBoth->setObjectName("ConflictBoth");
  mBoth->setToolTip(tr("Keep our lines followed by their lines"));
  connect(mBoth, &QPushButton::clicked, this,
          [this] { apply(Markers::Ours | Markers::Theirs); });

  mUndo = new QPushButton(tr("Undo"), this);
  mUndo->setObjectName("ConflictUndo");
  connect(mUndo, &QPushButton::clicked, this, [this] {
    mPanes[ResultPane]->undo();
    refresh();

    // Undo leaves the caret behind the block it brought back.
    if (mCurrent < 0 && !mBlocks.isEmpty()) {
      int caret = mPanes[ResultPane]->currentPos();
      int index = mBlocks.size() - 1;
      while (index > 0 && mBlocks.at(index).begin > caret)
        --index;
      showBlock(index);
    }
  });

  mSave = new QPushButton(tr("Save"), this);
  mSave->setObjectName("ConflictSave");
  mSave->setShortcut(QKeySequence::Save);
  mSave->setToolTip(tr("Save the file, and stage it when no conflicts are "
                       "left in it"));
  connect(mSave, &QPushButton::clicked, this,
          &ConflictDialog::saveAndContinue);

  QPushButton *close = new QPushButton(tr("Close"), this);
  close->setObjectName("ConflictClose");
  connect(close, &QPushButton::clicked, this, &ConflictDialog::reject);

  QHBoxLayout *bottom = new QHBoxLayout;
  bottom->addWidget(mOurs);
  bottom->addWidget(mBoth);
  bottom->addWidget(mTheirs);
  bottom->addWidget(mUndo);
  bottom->addStretch();
  bottom->addWidget(mSave);
  bottom->addWidget(close);

  for (QPushButton *button : findChildren<QPushButton *>())
    button->setAutoDefault(false);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(splitter, 1);
  layout->addLayout(bottom);

  mRefreshTimer = new QTimer(this);
  mRefreshTimer->setSingleShot(true);
  mRefreshTimer->setInterval(0);
  connect(mRefreshTimer, &QTimer::timeout, this, &ConflictDialog::refresh);

  // Markers can't be changed from inside of the change notification.
  connect(mPanes[ResultPane], &TextEditor::notifyChange, mRefreshTimer,
          qOverload<>(&QTimer::start));
  connect(mPanes[ResultPane], &TextEditor::updateUi, this,
          [this] { updateCurrent(); });

  resize(1400, 800);
  restoreGeometry(QSettings().value(kGeometryKey).toByteArray());

  loadFile(qMax(mFiles.indexOf(file), 0));
}

void ConflictDialog::done(int result) {
  QSettings().setValue(kGeometryKey, saveGeometry());
  QDialog::done(result);
}

void ConflictDialog::reject() {
  if (confirmDiscard())
    QDialog::reject();
}

TextEditor *ConflictDialog::addPane(Pane pane, QLabel *label) {
  TextEditor *editor = new TextEditor(this);
  label->setBuddy(editor);
  mPanes[pane] = editor;

  connect(editor, &TextEditor::verticalScrolled, this,
          [this, pane] { syncScroll(pane); });
  return editor;
}

void ConflictDialog::configurePane(TextEditor *editor) {
  // Scrolling goes by lines of the file, which must be lines on the screen.
  editor->setWrapMode(SC_WRAP_NONE);

  editor->setMarginWidthN(TextEditor::LineNumber,
                          editor->textWidth(STYLE_LINENUMBER, "_99999"));

  // A bar next to the lines of the current block.
  editor->markerDefine(TextEditor::Context, SC_MARK_FULLRECT);
  editor->markerSetBack(TextEditor::Context,
                        colour(palette().color(QPalette::Highlight)));
  editor->setMarginMaskN(TextEditor::Staged, 1 << TextEditor::Context);
  editor->setMarginWidthN(TextEditor::Staged, kCurrentBlockMargin);
}

void ConflictDialog::loadFile(int index) {
  mRefreshTimer->stop();
  mFileIndex = index;
  mFileBox->setCurrentIndex(index);

  QString name = mFiles.at(index);
  QString path = mRepo.workdir().filePath(name);

  QFile file(path);
  bool opened = file.open(QFile::ReadOnly);
  QByteArray bytes = opened ? file.readAll() : QByteArray();

  // Not every encoding can be configured, but the system one is likely used
  // by the tools that wrote a file that isn't in the configured one.
  mEncoding = mRepo.encoding();
  QString text = decodedWith(mEncoding, bytes);
  mEditable = opened && survives(mEncoding, text, bytes);
  if (opened && !mEditable) {
    QString system = decodedWith(QStringConverter::System, bytes);
    if (survives(QStringConverter::System, system, bytes)) {
      mEncoding = QStringConverter::System;
      text = system;
      mEditable = true;
    }
  }

  QByteArray utf8 = text.toUtf8();
  QList<Markers::Block> blocks = Markers::parse(utf8);
  Markers::Version ours = Markers::version(utf8, blocks, Markers::Ours);
  Markers::Version theirs = Markers::version(utf8, blocks, Markers::Theirs);

  mSyncing = true;
  for (TextEditor *editor : mPanes) {
    editor->setReadOnly(false); // Also blocks loading.
    editor->markerDeleteAll(TextEditor::Ours);
    editor->markerDeleteAll(TextEditor::Theirs);
    editor->markerDeleteAll(TextEditor::Context);
  }

  mPanes[OursPane]->load(path, QString::fromUtf8(ours.text));
  mPanes[TheirsPane]->load(path, QString::fromUtf8(theirs.text));
  mPanes[ResultPane]->load(path, text);

  // Loading applies the settings of the editor, which reset these.
  for (TextEditor *editor : mPanes)
    configurePane(editor);

  mPanes[OursPane]->setReadOnly(true);
  mPanes[TheirsPane]->setReadOnly(true);
  mPanes[ResultPane]->setReadOnly(!mEditable);
  mSyncing = false;

  mOrigins.clear();
  for (int i = 0; i < blocks.size(); ++i) {
    const Markers::Block &block = blocks.at(i);
    TextEditor *oursPane = mPanes[OursPane];
    TextEditor *theirsPane = mPanes[TheirsPane];
    Origin origin{Markers::resolution(utf8, block, Markers::Ours),
                  Markers::resolution(utf8, block, Markers::Theirs),
                  static_cast<int>(
                      oursPane->lineFromPosition(ours.blocks.at(i).begin)),
                  static_cast<int>(
                      oursPane->lineFromPosition(ours.blocks.at(i).end)),
                  static_cast<int>(
                      theirsPane->lineFromPosition(theirs.blocks.at(i).begin)),
                  static_cast<int>(
                      theirsPane->lineFromPosition(theirs.blocks.at(i).end))};
    mOrigins.append(origin);
    addMarker(oursPane, TextEditor::Ours, origin.oursBegin, origin.oursEnd);
    addMarker(theirsPane, TextEditor::Theirs, origin.theirsBegin,
              origin.theirsEnd);
  }

  QByteArray oursName =
      blocks.isEmpty() ? QByteArray() : blocks.first().oursLabel;
  QByteArray theirsName =
      blocks.isEmpty() ? QByteArray() : blocks.first().theirsLabel;
  mOursLabel->setText(oursName.isEmpty()
                          ? tr("Ours")
                          : tr("Ours: %1").arg(QString::fromUtf8(oursName)));
  mTheirsLabel->setText(
      theirsName.isEmpty()
          ? tr("Theirs")
          : tr("Theirs: %1").arg(QString::fromUtf8(theirsName)));

  mCurrent = -1;
  refresh();
  if (!mBlocks.isEmpty())
    showBlock(0);
}

void ConflictDialog::selectFile(int index) {
  if (index == mFileIndex)
    return;

  if (confirmDiscard())
    loadFile(index);
  else
    mFileBox->setCurrentIndex(mFileIndex);
}

QByteArray ConflictDialog::resultText() const {
  TextEditor *editor = mPanes[ResultPane];
  return editor->textRange(0, editor->textLength());
}

void ConflictDialog::refresh() {
  mRefreshTimer->stop();

  TextEditor *result = mPanes[ResultPane];
  mBlocks = Markers::parse(resultText());
  result->markerDeleteAll(TextEditor::Ours);
  result->markerDeleteAll(TextEditor::Theirs);

  QByteArray text = resultText();
  QList<int> resultOf(mOrigins.size(), -1);
  mMatches.clear();

  // Blocks that were solved are gone, and the others still are in order.
  int from = 0;
  for (int i = 0; i < mBlocks.size(); ++i) {
    const Markers::Block &block = mBlocks.at(i);
    addMarker(result, TextEditor::Ours,
              result->lineFromPosition(block.ours.begin),
              result->lineFromPosition(block.ours.end));
    addMarker(result, TextEditor::Theirs,
              result->lineFromPosition(block.theirs.begin),
              result->lineFromPosition(block.theirs.end));

    QByteArray ours = Markers::resolution(text, block, Markers::Ours);
    QByteArray theirs = Markers::resolution(text, block, Markers::Theirs);
    int match = -1;
    for (int j = from; j < mOrigins.size() && match < 0; ++j) {
      if (mOrigins.at(j).ours == ours && mOrigins.at(j).theirs == theirs)
        match = j;
    }

    mMatches.append(match);
    if (match >= 0) {
      resultOf[match] = i;
      from = match + 1;
    }
  }

  // The panes stay lined up at the blocks when scrolling.
  mAnchors.clear();
  for (int i = 0; i < mOrigins.size(); ++i) {
    const Origin &origin = mOrigins.at(i);
    int block = resultOf.at(i);
    int begin = -1;
    int end = -1;
    if (block >= 0) {
      begin = result->lineFromPosition(mBlocks.at(block).begin);
      end = result->lineFromPosition(mBlocks.at(block).end);
    }
    mAnchors.append({origin.oursBegin, begin, origin.theirsBegin});
    mAnchors.append({origin.oursEnd, end, origin.theirsEnd});
  }

  mUndo->setEnabled(mEditable && result->canUndo());
  mSave->setEnabled(mEditable);
  updateCurrent(true);
}

void ConflictDialog::updateCurrent(bool force) {
  TextEditor *result = mPanes[ResultPane];
  int caret = result->currentPos();
  int current = -1;
  for (int i = 0; i < mBlocks.size() && current < 0; ++i) {
    if (caret >= mBlocks.at(i).begin && caret < mBlocks.at(i).end)
      current = i;
  }

  if (!force && current == mCurrent)
    return;

  mCurrent = current;
  for (TextEditor *editor : mPanes)
    editor->markerDeleteAll(TextEditor::Context);

  if (mCurrent >= 0) {
    const Markers::Block &block = mBlocks.at(mCurrent);
    addMarker(result, TextEditor::Context,
              result->lineFromPosition(block.begin),
              result->lineFromPosition(block.end));

    int match = mMatches.at(mCurrent);
    if (match >= 0) {
      // A side without any lines still shows where the block is.
      const Origin &origin = mOrigins.at(match);
      addMarker(mPanes[OursPane], TextEditor::Context, origin.oursBegin,
                qMax(origin.oursEnd, origin.oursBegin + 1));
      addMarker(mPanes[TheirsPane], TextEditor::Context, origin.theirsBegin,
                qMax(origin.theirsEnd, origin.theirsBegin + 1));
    }
  }

  bool solvable = mEditable && mCurrent >= 0;
  mOurs->setEnabled(solvable);
  mTheirs->setEnabled(solvable);
  mBoth->setEnabled(solvable);
  mPrevious->setEnabled(!mBlocks.isEmpty());
  mNext->setEnabled(!mBlocks.isEmpty());

  if (!mEditable) {
    mStatus->setText(tr("Can't be saved: unknown encoding of the file"));
  } else if (mBlocks.isEmpty()) {
    mStatus->setText(tr("No conflicts left in the file"));
  } else if (mCurrent >= 0) {
    mStatus->setText(
        tr("Conflict %1 of %2").arg(mCurrent + 1).arg(mBlocks.size()));
  } else {
    mStatus->setText(tr("Unresolved conflicts: %1").arg(mBlocks.size()));
  }
}

void ConflictDialog::showBlock(int index) {
  TextEditor *result = mPanes[ResultPane];
  const Markers::Block &block = mBlocks.at(index);
  int line = result->lineFromPosition(block.begin);

  mSyncing = true;
  result->gotoPos(block.begin);
  updateCurrent();
  result->setFirstVisibleLine(qMax(line - kContextLines, 0));

  int match = mMatches.at(index);
  if (match >= 0) {
    const Origin &origin = mOrigins.at(match);
    mPanes[OursPane]->setFirstVisibleLine(
        qMax(origin.oursBegin - kContextLines, 0));
    mPanes[TheirsPane]->setFirstVisibleLine(
        qMax(origin.theirsBegin - kContextLines, 0));
  }
  mSyncing = false;

  result->QWidget::setFocus();
}

void ConflictDialog::next() {
  if (mBlocks.isEmpty())
    return;

  int caret = mPanes[ResultPane]->currentPos();
  int index = mCurrent + 1;
  if (mCurrent < 0) {
    index = 0;
    while (index < mBlocks.size() && mBlocks.at(index).begin <= caret)
      ++index;
  }

  showBlock(index % mBlocks.size());
}

void ConflictDialog::previous() {
  if (mBlocks.isEmpty())
    return;

  int caret = mPanes[ResultPane]->currentPos();
  int index = mCurrent - 1;
  if (mCurrent < 0) {
    index = mBlocks.size() - 1;
    while (index >= 0 && mBlocks.at(index).end > caret)
      --index;
  }

  showBlock((index + mBlocks.size()) % mBlocks.size());
}

void ConflictDialog::apply(int sides) {
  if (mCurrent < 0)
    return;

  // Blocks are parsed on a timer after an edit: never replace stale offsets.
  refresh();
  if (mCurrent < 0)
    return;

  Markers::Block block = mBlocks.at(mCurrent);
  QByteArray text = Markers::resolution(resultText(), block, sides);

  TextEditor *result = mPanes[ResultPane];
  result->setTargetRange(block.begin, block.end);
  result->replaceTarget(text.size(), text.constData());
  result->gotoPos(block.begin);
  refresh();

  if (mBlocks.isEmpty()) {
    mSave->setFocus();
    return;
  }

  int index = 0;
  while (index < mBlocks.size() && mBlocks.at(index).begin < block.begin)
    ++index;
  showBlock(index % mBlocks.size());
}

bool ConflictDialog::save() {
  refresh();

  QString name = mFiles.at(mFileIndex);
  QString text = QString::fromUtf8(resultText());
  QByteArray bytes = encodedWith(mEncoding, text);
  if (decodedWith(mEncoding, bytes) != text) {
    QMessageBox::warning(
        this, tr("Save Failed"),
        tr("The result has characters that the encoding of %1 doesn't have.")
            .arg(name));
    return false;
  }

  QSaveFile file(mRepo.workdir().filePath(name));
  if (!file.open(QFile::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    QMessageBox::warning(this, tr("Save Failed"),
                         tr("Unable to write %1: %2")
                             .arg(name, file.errorString()));
    return false;
  }

  mPanes[ResultPane]->setSavePoint();

  // They are kept by line numbers, which aren't valid anymore.
  git::Patch::clearConflictResolutions(mRepo, name);

  if (mBlocks.isEmpty())
    mRepo.index().setStaged({name}, true);

  emit saved();
  return true;
}

void ConflictDialog::saveAndContinue() {
  if (!save() || !mBlocks.isEmpty())
    return;

  // This file is done.
  int index = mFileIndex;
  mFiles.removeAt(index);
  mFileBox->removeItem(index);
  mFileIndex = -1;

  if (mFiles.isEmpty()) {
    accept();
    return;
  }

  loadFile(qMin(index, mFiles.size() - 1));
}

bool ConflictDialog::confirmDiscard() {
  if (!mPanes[ResultPane]->modify())
    return true;

  QMessageBox::StandardButton button = QMessageBox::question(
      this, tr("Unsaved Changes"),
      tr("Save the changes of %1?").arg(mFiles.at(mFileIndex)),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

  if (button == QMessageBox::Save)
    return save();
  return button == QMessageBox::Discard;
}

void ConflictDialog::syncScroll(Pane from) {
  if (mSyncing)
    return;

  mSyncing = true;
  int line = mPanes[from]->firstVisibleLine();
  for (int to = 0; to < PaneCount; ++to) {
    if (to != from)
      mPanes[to]->setFirstVisibleLine(
          mapLine(from, static_cast<Pane>(to), line));
  }
  mSyncing = false;
}

int ConflictDialog::mapLine(Pane from, Pane to, int line) {
  // Goes straight between the anchors that both panes have.
  auto between = [line](int a0, int b0, int a1, int b1) {
    return a1 > a0 ? b0 + (line - a0) * (b1 - b0) / (a1 - a0) : b0;
  };

  int a0 = 0;
  int b0 = 0;
  for (const Anchor &anchor : mAnchors) {
    int a = anchor[from];
    int b = anchor[to];
    if (a < 0 || b < 0)
      continue;

    if (line < a)
      return between(a0, b0, a, b);

    a0 = a;
    b0 = b;
  }

  return between(a0, b0, mPanes[from]->lineCount(), mPanes[to]->lineCount());
}
