//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Signing.h"
#include "Command.h"
#include "Config.h"
#include "git2.h"
#include "git2/sys/errors.h"
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace git {

namespace {

QString format(const Config &config) {
  QString format = config.value<QString>("gpg.format").toLower();
  return format.isEmpty() ? QString("openpgp") : format;
}

// The program for the format, found like git finds it.
QString program(const Config &config, const QString &format) {
  QString name = config.value<QString>(QString("gpg.%1.program").arg(format));
  if (name.isEmpty() && format == "openpgp")
    name = config.value<QString>("gpg.program");
  if (name.isEmpty())
    name = (format == "ssh") ? "ssh-keygen"
           : (format == "x509") ? "gpgsm"
                                : "gpg";

  if (QFileInfo(name).isAbsolute())
    return name;

  QString path = QStandardPaths::findExecutable(name);
#ifdef Q_OS_WIN
  // Git for Windows comes with gpg and ssh-keygen.
  if (path.isEmpty()) {
    QString git = Command::gitPath();
    if (!git.isEmpty()) {
      QDir dir = QFileInfo(git).dir();
      dir.cdUp();
      path = QStandardPaths::findExecutable(name, {dir.filePath("usr/bin")});
    }
  }
#endif

  return path.isEmpty() ? name : path;
}

QByteArray signGpg(const QString &program, const QString &key,
                   const QByteArray &data, QString *error) {
  QProcess process;
  process.start(program, {"--status-fd=2", "-bsau", key});
  if (!process.waitForStarted(-1)) {
    *error = QObject::tr("Unable to run %1: %2")
                 .arg(program, process.errorString());
    return QByteArray();
  }

  process.write(data);
  process.closeWriteChannel();
  process.waitForFinished(-1); // may wait for a passphrase

  QByteArray signature = process.readAllStandardOutput();
  QString status = QString::fromUtf8(process.readAllStandardError());
  if (process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0 || !status.contains("[GNUPG:] SIG_CREATED ")) {
    *error = QObject::tr("%1 failed to sign the data: %2")
                 .arg(program, status.trimmed());
    return QByteArray();
  }

  return signature;
}

QByteArray signSsh(const QString &program, const QString &signingKey,
                   const QByteArray &data, QString *error) {
  if (signingKey.isEmpty()) {
    *error = QObject::tr("user.signingKey needs to be set for ssh signing.");
    return QByteArray();
  }

  // A literal public key is used with the ssh agent.
  QString key = signingKey;
  bool literal = false;
  if (key.startsWith("key::")) {
    key.remove(0, 5);
    literal = true;
  } else if (key.startsWith("ssh-")) {
    literal = true;
  }

  QTemporaryFile keyFile(QDir::temp().filePath("git_signing_key_XXXXXX"));
  QString keyPath;
  if (literal) {
    if (!keyFile.open() || keyFile.write(key.toUtf8()) < 0) {
      *error = QObject::tr("Unable to write the ssh signing key.");
      return QByteArray();
    }
    keyFile.close();
    keyPath = keyFile.fileName();
  } else {
    keyPath = key.startsWith("~/") ? QDir::home().filePath(key.mid(2)) : key;
  }

  QTemporaryFile buffer(QDir::temp().filePath("git_signing_buffer_XXXXXX"));
  if (!buffer.open() || buffer.write(data) < 0) {
    *error = QObject::tr("Unable to write the data to sign.");
    return QByteArray();
  }
  buffer.close();

  QStringList args = {"-Y", "sign", "-n", "git", "-f",
                      QDir::toNativeSeparators(keyPath)};
  if (literal)
    args.append("-U");
  args.append(QDir::toNativeSeparators(buffer.fileName()));

  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(program, args);
  if (!process.waitForStarted(-1)) {
    *error = QObject::tr("Unable to run %1: %2")
                 .arg(program, process.errorString());
    return QByteArray();
  }

  process.closeWriteChannel();
  process.waitForFinished(-1); // may wait for a passphrase

  QFile file(buffer.fileName() + ".sig");
  QByteArray signature;
  if (file.open(QFile::ReadOnly))
    signature = file.readAll();
  file.close();
  file.remove();

  if (process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0 || signature.isEmpty()) {
    QString output = QString::fromUtf8(process.readAll()).trimmed();
    *error = QObject::tr("%1 failed to sign the data: %2").arg(program, output);
    return QByteArray();
  }

  return signature;
}

} // namespace

bool Signing::isEnabled(const Config &config) {
  return config.value<bool>("commit.gpgSign", false);
}

QByteArray Signing::sign(const Config &config, const QByteArray &data,
                         const QString &committer, QString *error) {
  QString ignored;
  if (!error)
    error = &ignored;

  QString fmt = format(config);
  QString key = config.value<QString>("user.signingKey");
  if (fmt == "ssh")
    return signSsh(program(config, fmt), key, data, error);

  if (fmt != "openpgp" && fmt != "x509") {
    *error = QObject::tr("The signing format '%1' isn't supported.").arg(fmt);
    return QByteArray();
  }

  return signGpg(program(config, fmt), key.isEmpty() ? committer : key, data,
                 error);
}

int Signing::createCommit(git_oid *id, git_repository *repo,
                          const Config &config, const git_signature *author,
                          const git_signature *committer,
                          const QString &message, const git_tree *tree,
                          size_t parentCount, const git_commit **parents,
                          const QString &action) {
  QByteArray text = message.toUtf8();
  git_buf buf = GIT_BUF_INIT;
  int error = git_commit_create_buffer(&buf, repo, author, committer, nullptr,
                                       text, tree, parentCount, parents);
  if (error)
    return error;

  QByteArray data(buf.ptr, static_cast<qsizetype>(buf.size));
  git_buf_dispose(&buf);

  QString signError;
  QString name = QString("%1 <%2>").arg(QString::fromUtf8(committer->name),
                                        QString::fromUtf8(committer->email));
  QByteArray signature = sign(config, data, name, &signError);
  if (signature.isEmpty()) {
    git_error_set_str(GIT_ERROR_INVALID, signError.toUtf8());
    return -1;
  }

  if ((error = git_commit_create_with_signature(id, repo, data, signature,
                                                nullptr)))
    return error;

  // Move HEAD, or the branch that it points to, like git_commit_create().
  QString kind = action;
  if (kind.isEmpty() && parentCount == 0)
    kind = "initial";
  else if (kind.isEmpty() && parentCount > 1)
    kind = "merge";

  QString summary = message.section('\n', 0, 0);
  QString log = kind.isEmpty() ? QString("commit: %1").arg(summary)
                               : QString("commit (%1): %2").arg(kind, summary);

  git_reference *head = nullptr;
  if ((error = git_reference_lookup(&head, repo, "HEAD")))
    return error;

  QByteArray target = (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC)
                          ? QByteArray(git_reference_symbolic_target(head))
                          : QByteArray("HEAD");
  git_reference_free(head);

  git_reference *ref = nullptr;
  error = git_reference_create(&ref, repo, target, id, 1, log.toUtf8());
  git_reference_free(ref);
  return error;
}

} // namespace git
