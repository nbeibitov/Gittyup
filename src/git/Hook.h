//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef HOOK_H
#define HOOK_H

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace git {

// Runs the hooks of a repository like git does: from core.hooksPath or the
// hooks directory of the common git directory, in the working directory.
// Opens its own repository handle, so it can run on any thread.
class Hook {
public:
  struct Result {
    bool found = false; // the hook exists and was run
    int exitCode = 0;
    QString output; // standard output and error
    QString error;  // the hook couldn't be run

    bool succeeded() const { return error.isEmpty() && exitCode == 0; }
  };

  // The directory that the hooks of the repository are looked up in.
  static QString dir(const QString &gitDir);

  // The path of the hook, or an empty string if there is none. On Unix
  // a hook has to be executable, like git requires.
  static QString path(const QString &gitDir, const QString &name);

  // Run the hook with the arguments, writing input to its standard input.
  // env is added to the environment of the process. A missing hook
  // succeeds.
  static Result run(const QString &gitDir, const QString &name,
                    const QStringList &args = QStringList(),
                    const QByteArray &input = QByteArray(),
                    const QProcessEnvironment &env = QProcessEnvironment());
};

} // namespace git

#endif
