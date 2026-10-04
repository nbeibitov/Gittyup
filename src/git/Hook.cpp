//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "Hook.h"
#include "Command.h"
#include "git2.h"
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <memory>

namespace git {

namespace {

struct Paths {
  QString hooks;
  QString workdir; // where hooks run
};

bool paths(const QString &gitDir, Paths &out) {
  git_repository *raw = nullptr;
  if (git_repository_open_ext(&raw, gitDir.toUtf8(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr))
    return false;

  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(
      raw, git_repository_free);

  // Hooks run in the root of the working tree, or in the git directory of a
  // bare repository.
  const char *workdir = git_repository_workdir(repo.get());
  out.workdir = QString::fromUtf8(workdir ? workdir
                                          : git_repository_path(repo.get()));

  git_config *config = nullptr;
  if (!git_repository_config_snapshot(&config, repo.get())) {
    git_buf buf = GIT_BUF_INIT;
    if (!git_config_get_path(&buf, config, "core.hooksPath")) {
      QString path = QString::fromUtf8(buf.ptr, static_cast<int>(buf.size));
      out.hooks = QDir(out.workdir).absoluteFilePath(path);
    }
    git_buf_dispose(&buf);
    git_config_free(config);
  }

  // Linked worktrees share the hooks of the main repository.
  if (out.hooks.isEmpty())
    out.hooks =
        QDir(QString::fromUtf8(git_repository_commondir(repo.get())))
            .filePath("hooks");

  return true;
}

} // namespace

QString Hook::dir(const QString &gitDir) {
  Paths result;
  return paths(gitDir, result) ? result.hooks : QString();
}

QString Hook::path(const QString &gitDir, const QString &name) {
  QString dir = Hook::dir(gitDir);
  if (dir.isEmpty())
    return QString();

  QFileInfo info(QDir(dir).filePath(name));
  if (!info.isFile())
    return QString();

#ifndef Q_OS_WIN
  if (!info.isExecutable())
    return QString(); // git ignores it, too
#endif

  return info.filePath();
}

Hook::Result Hook::run(const QString &gitDir, const QString &name,
                       const QStringList &args, const QByteArray &input,
                       const QProcessEnvironment &env) {
  Result result;
  Paths dirs;
  if (!paths(gitDir, dirs)) {
    result.error = QObject::tr("Unable to open the repository.");
    return result;
  }

  QString hook = path(gitDir, name);
  if (hook.isEmpty())
    return result;

  result.found = true;

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  for (const QString &key : env.keys())
    environment.insert(key, env.value(key));

  QProcess process;
  process.setProcessEnvironment(environment);
  process.setWorkingDirectory(dirs.workdir);
  process.setProcessChannelMode(QProcess::MergedChannels);

#ifdef Q_OS_WIN
  // The shell of Git for Windows runs scripts with any interpreter of their
  // first line, like git does.
  QString bash = Command::bashPath();
  if (bash.isEmpty()) {
    result.error =
        QObject::tr("Unable to run the %1 hook: bash was not found.").arg(name);
    return result;
  }

  process.start(bash, QStringList({"-c", "exec \"$0\" \"$@\"", hook}) + args);
#else
  process.start(hook, args);
#endif

  if (!process.waitForStarted(-1)) {
    result.error = QObject::tr("Unable to run the %1 hook: %2")
                       .arg(name, process.errorString());
    return result;
  }

  process.write(input);
  process.closeWriteChannel();
  process.waitForFinished(-1);

  result.output = QString::fromUtf8(process.readAll()).trimmed();
  if (process.exitStatus() != QProcess::NormalExit) {
    result.error = QObject::tr("The %1 hook crashed.").arg(name);
    return result;
  }

  result.exitCode = process.exitCode();
  return result;
}

} // namespace git
