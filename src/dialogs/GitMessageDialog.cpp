//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "GitMessageDialog.h"
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

GitMessageDialog::GitMessageDialog(const QString &text, QChar commentChar,
                                   QWidget *parent)
    : QDialog(parent), mCommentChar(commentChar) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Commit Message"));
  resize(640, 420);

  // Git removes its comments from the message (commit.cleanup=strip).
  QStringList message;
  QStringList comments;
  QString info;
  for (const QString &line : text.split('\n')) {
    if (line.startsWith(commentChar)) {
      comments.append(line);
      QString hint = line.mid(1).trimmed();
      if (!hint.isEmpty())
        info.append(hint + '\n');
    } else {
      message.append(line);
    }
  }

  // Squashed messages contain comments between the parts; drop the blank
  // lines that surround them at the ends.
  while (!message.isEmpty() && message.first().trimmed().isEmpty())
    message.removeFirst();
  while (!message.isEmpty() && message.last().trimmed().isEmpty())
    message.removeLast();
  mComments = comments.join('\n');

  QLabel *label = new QLabel(
      tr("Git asks for the message of the commit. Lines starting with '%1' "
         "are ignored.")
          .arg(commentChar),
      this);
  label->setWordWrap(true);

  mMessage = new QPlainTextEdit(this);
  mMessage->setObjectName("GitMessage");
  mMessage->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  mMessage->setPlainText(message.join('\n'));
  mMessage->setTabChangesFocus(true);

  QPlainTextEdit *hints = new QPlainTextEdit(info.trimmed(), this);
  hints->setReadOnly(true);
  hints->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  hints->setMaximumHeight(110);
  hints->setVisible(!info.trimmed().isEmpty());

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  mAccept = buttons->button(QDialogButtonBox::Ok);
  mAccept->setText(tr("Use Message"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  // Git aborts the commit if the message is empty.
  auto update = [this] {
    mAccept->setEnabled(!this->message().trimmed().isEmpty());
  };
  connect(mMessage, &QPlainTextEdit::textChanged, this, update);
  update();

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(label);
  layout->addWidget(mMessage, 1);
  layout->addWidget(hints);
  layout->addWidget(buttons);

  mMessage->setFocus();
}

QString GitMessageDialog::text() const {
  QString text = message().trimmed() + '\n';
  if (!mComments.isEmpty())
    text += '\n' + mComments + '\n';
  return text;
}

QString GitMessageDialog::message() const { return mMessage->toPlainText(); }

void GitMessageDialog::setMessage(const QString &message) {
  mMessage->setPlainText(message);
}
