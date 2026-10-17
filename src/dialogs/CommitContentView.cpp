//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "CommitContentView.h"
#include "app/Application.h"
#include "app/Theme.h"
#include "git/Diff.h"
#include "git/Patch.h"
#include "git/Signature.h"
#include <QFontDatabase>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QVBoxLayout>

namespace {

// Keeps big commits responsive.
const int kMaxFileLines = 2000;
const int kMaxLines = 20000;

// Lines of a patch start with their origin; other lines name a file.
class PatchHighlighter : public QSyntaxHighlighter {
public:
  PatchHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
    Theme *theme = Application::theme();
    mAddition.setBackground(theme->diff(Theme::Diff::Addition));
    mDeletion.setBackground(theme->diff(Theme::Diff::Deletion));
    mHunk.setForeground(QPalette().brush(QPalette::Disabled, QPalette::Text));
    mFile.setFontWeight(QFont::Bold);
  }

protected:
  void highlightBlock(const QString &text) override {
    if (text.isEmpty())
      return;

    switch (text.at(0).unicode()) {
      case '+':
        setFormat(0, text.length(), mAddition);
        break;
      case '-':
        setFormat(0, text.length(), mDeletion);
        break;
      case '@':
      case '\\':
        setFormat(0, text.length(), mHunk);
        break;
      case ' ':
        break;
      default:
        setFormat(0, text.length(), mFile);
        break;
    }
  }

private:
  QTextCharFormat mAddition;
  QTextCharFormat mDeletion;
  QTextCharFormat mHunk;
  QTextCharFormat mFile;
};

QString chopNewline(QByteArray content) {
  while (content.endsWith('\n') || content.endsWith('\r'))
    content.chop(1);
  return QString::fromUtf8(content);
}

} // namespace

CommitContentView::CommitContentView(QWidget *parent) : QWidget(parent) {
  mHeader = new QLabel(this);
  mHeader->setObjectName("CommitContentHeader");
  mHeader->setWordWrap(true);
  mHeader->setTextFormat(Qt::RichText);
  mHeader->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mHeader->setAlignment(Qt::AlignTop | Qt::AlignLeft);

  mFiles = new QListWidget(this);
  mFiles->setObjectName("CommitContentFiles");

  QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mPatch = new QPlainTextEdit(this);
  mPatch->setObjectName("CommitContentPatch");
  mPatch->setReadOnly(true);
  mPatch->setLineWrapMode(QPlainTextEdit::NoWrap);
  mPatch->setFont(fixed);
  new PatchHighlighter(mPatch->document());

  // A file jumps to its patch.
  connect(mFiles, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row < 0 || row >= mFileBlocks.size())
      return;
    QTextBlock block = mPatch->document()->findBlockByNumber(mFileBlocks.at(row));
    QTextCursor cursor(block);
    mPatch->setTextCursor(cursor);
    QScrollBar *bar = mPatch->verticalScrollBar();
    bar->setValue(qMin(block.blockNumber(), bar->maximum()));
  });

  QSplitter *splitter = new QSplitter(Qt::Vertical, this);
  splitter->setChildrenCollapsible(false);
  QScrollArea *header = new QScrollArea(this);
  header->setWidgetResizable(true);
  header->setFrameShape(QFrame::NoFrame);
  header->setWidget(mHeader);
  splitter->addWidget(header);
  splitter->addWidget(mFiles);
  splitter->addWidget(mPatch);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 4);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(splitter);

  setCommit(git::Commit());
}

void CommitContentView::setCommit(const git::Commit &commit,
                                  const QString &text) {
  if (commit.isValid() && mCommit.isValid() && commit.id() == mCommit.id())
    return;

  mCommit = commit;
  mFiles->clear();
  mPatch->clear();
  mFileBlocks.clear();

  if (!commit.isValid()) {
    mHeader->setText(text.toHtmlEscaped());
    return;
  }

  // Author, date and the whole message.
  git::Signature author = commit.author();
  QString date = QLocale().toString(author.date(), QLocale::LongFormat);
  QString header =
      QString("<b>%1</b> &nbsp;%2 &lt;%3&gt;<br>%4")
          .arg(commit.shortId(), author.name().toHtmlEscaped(),
               author.email().toHtmlEscaped(), date.toHtmlEscaped());
  QList<git::Commit> parents = commit.parents();
  if (parents.size() > 1)
    header += "<br><i>" +
              tr("Merge: changes compared with the first parent %1")
                  .arg(parents.first().shortId()) +
              "</i>";
  QString message = commit.message().trimmed().toHtmlEscaped();
  message.replace('\n', "<br>");
  mHeader->setText(header + "<br><br>" + message);

  git::Diff diff = commit.diff();
  if (!diff.isValid())
    return;
  diff.findSimilar();

  QStringList lines;
  for (int i = 0; i < diff.count(); ++i) {
    git::Patch patch = diff.patch(i);
    char status = git::Diff::statusChar(diff.status(i));
    QString name = diff.name(i);
    QString oldName = patch.isValid() ? patch.name(git::Diff::OldFile) : name;
    QString title = (oldName != name && !oldName.isEmpty())
                        ? QString("%1 %2 %3").arg(oldName, QChar(0x2192), name)
                        : name;

    QString stats;
    if (patch.isValid() && !diff.isBinary(i)) {
      git::Patch::LineStats lineStats = patch.lineStats();
      stats = QString("  +%1 %2%3").arg(lineStats.additions)
                  .arg(QChar(0x2212))
                  .arg(lineStats.deletions);
    }
    mFiles->addItem(QString("%1  %2%3").arg(QChar(status), title, stats));

    if (!lines.isEmpty())
      lines.append(QString());
    mFileBlocks.append(lines.size());
    lines.append(QString("%1 %2").arg(QChar(status), title));

    if (lines.size() >= kMaxLines) {
      lines.append(tr("(The rest of the patch is too long to show.)"));
      continue;
    }

    if (diff.isBinary(i) || !patch.isValid()) {
      lines.append(" " + tr("Binary file"));
      continue;
    }

    int fileLines = 0;
    int skipped = 0;
    for (int hidx = 0; hidx < patch.count(); ++hidx) {
      int count = patch.lineCount(hidx);
      if (fileLines >= kMaxFileLines || lines.size() >= kMaxLines) {
        skipped += count;
        continue;
      }

      lines.append(chopNewline(patch.header(hidx)));
      for (int line = 0; line < count; ++line) {
        char origin = patch.lineOrigin(hidx, line);
        QString content = chopNewline(patch.lineContent(hidx, line));
        switch (origin) {
          case GIT_DIFF_LINE_ADDITION:
          case GIT_DIFF_LINE_DELETION:
          case GIT_DIFF_LINE_CONTEXT:
            lines.append(QChar(origin) + content);
            break;
          default: // the end of a file without newline
            lines.append("\\ " + tr("No newline at end of file"));
            break;
        }
      }
      fileLines += count;
    }

    if (skipped)
      lines.append(" " + tr("(%1 more lines not shown)").arg(skipped));
  }

  mPatch->setPlainText(lines.join('\n'));
}

QStringList CommitContentView::files() const {
  QStringList result;
  for (int row = 0; row < mFiles->count(); ++row)
    result.append(mFiles->item(row)->text());
  return result;
}

QString CommitContentView::patch() const { return mPatch->toPlainText(); }
