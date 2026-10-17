//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "GitEditor.h"
#include <QCoreApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUuid>

namespace {

// Git runs the editor with sh.
QString shellQuote(const QString &arg) {
  QString quoted = arg;
  quoted.replace("'", "'\\''");
  return QString("'%1'").arg(quoted);
}

} // namespace

GitEditor::GitEditor(Handler handler, QObject *parent)
    : QObject(parent), mHandler(std::move(handler)),
      mServer(new QLocalServer(this)), mHelper(helperPath()) {
  mServer->setSocketOptions(QLocalServer::UserAccessOption);
  connect(mServer, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket *socket = mServer->nextPendingConnection()) {
      connect(socket, &QLocalSocket::disconnected, socket,
              &QObject::deleteLater);
      connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        if (!socket->canReadLine())
          return;

        // <kind>\t<file>\n
        QString line = QString::fromUtf8(socket->readLine()).trimmed();
        QString kind = line.section('\t', 0, 0);
        QString file = line.section('\t', 1);

        // The dialog may outlive the server or the helper.
        QPointer<GitEditor> self(this);
        QPointer<QLocalSocket> guard(socket);
        Reply reply = [self, guard](bool ok) {
          if (!ok && self)
            self->mCanceled = true;
          if (!guard)
            return;
          guard->write(ok ? "ok\n" : "cancel\n");
          guard->flush();
          guard->disconnectFromServer();
        };

        if (file.isEmpty() || (kind != "sequence" && kind != "message")) {
          reply(false);
          return;
        }

        mHandler(kind == "sequence" ? Kind::Sequence : Kind::Message,
                 QDir::fromNativeSeparators(file), reply);
      });
    }
  });
}

GitEditor::~GitEditor() { mServer->close(); }

QString GitEditor::helperPath() {
  QDir dir(QCoreApplication::applicationDirPath());
  QStringList paths = {dir.path()};
  if (dir.cdUp())
    paths.append(dir.path());
  return QStandardPaths::findExecutable("gittyup-editor", paths);
}

QString GitEditor::listen() {
  if (mHelper.isEmpty())
    return tr("The helper gittyup-editor was not found next to Gittyup.");

  QString name = QString("gittyup-editor-%1")
                     .arg(QUuid::createUuid().toString(QUuid::Id128));
  if (!mServer->listen(name))
    return mServer->errorString();

  return QString();
}

void GitEditor::setup(QProcessEnvironment &env) const {
  env.insert("GIT_SEQUENCE_EDITOR", command("sequence"));
  env.insert("GIT_EDITOR", command("message"));
}

bool GitEditor::takeCanceled() {
  bool canceled = mCanceled;
  mCanceled = false;
  return canceled;
}

QString GitEditor::command(const QString &kind) const {
  return QString("%1 %2 %3")
      .arg(shellQuote(QDir::fromNativeSeparators(mHelper)), kind,
           shellQuote(mServer->fullServerName()));
}
