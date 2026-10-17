//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

// The editor that Gittyup gives to git (GIT_EDITOR, GIT_SEQUENCE_EDITOR).
// Git runs it as: gittyup-editor <kind> <server> <file>
// It asks the running Gittyup to show the file in a dialog and waits for the
// answer. The exit code tells git whether the file was accepted.

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocalSocket>

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  QStringList args = app.arguments();
  if (args.size() != 4)
    return 2;

  QLocalSocket socket;
  socket.connectToServer(args.at(2));
  if (!socket.waitForConnected(10000))
    return 3;

  // Git may pass a path relative to the working directory.
  QString file = QFileInfo(args.at(3)).absoluteFilePath();
  socket.write(QString("%1\t%2\n").arg(args.at(1), file).toUtf8());
  if (!socket.waitForBytesWritten(10000))
    return 3;

  // The user may take as long as they like in the dialog.
  QByteArray reply;
  while (!reply.contains('\n')) {
    if (!socket.waitForReadyRead(-1) && !socket.bytesAvailable())
      return 1;
    reply.append(socket.readAll());
  }

  return reply.startsWith("ok\n") ? 0 : 1;
}
