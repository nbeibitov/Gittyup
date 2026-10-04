//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef SIGNING_H
#define SIGNING_H

#include <QByteArray>
#include <QString>

struct git_commit;
struct git_oid;
struct git_repository;
struct git_signature;
struct git_tree;

namespace git {

class Config;

// Signs commits like git does with commit.gpgSign: with gpg (gpg.format
// openpgp, the default), gpgsm (x509) or ssh-keygen (ssh), the key of
// user.signingKey and the programs of gpg.program and gpg.<format>.program.
class Signing {
public:
  static bool isEnabled(const Config &config);

  // The signature of data, or an empty array if signing failed. committer
  // ("Name <email>") is the key if user.signingKey isn't set, like in git.
  static QByteArray sign(const Config &config, const QByteArray &data,
                         const QString &committer, QString *error = nullptr);

  // Create a signed commit like git_commit_create() and move HEAD to it,
  // with a reflog message like git's ("commit: <summary>"). action is the
  // part in parentheses, e.g. "amend". Sets the libgit2 error on failure.
  static int createCommit(git_oid *id, git_repository *repo,
                          const Config &config, const git_signature *author,
                          const git_signature *committer,
                          const QString &message, const git_tree *tree,
                          size_t parentCount, const git_commit **parents,
                          const QString &action = QString());
};

} // namespace git

#endif
