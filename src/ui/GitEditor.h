//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef GITEDITOR_H
#define GITEDITOR_H

#include <QObject>
#include <functional>

class QLocalServer;
class QProcessEnvironment;

// Makes git open the files it wants edited (the todo list of an interactive
// rebase, commit messages) in dialogs of Gittyup instead of a text editor.
// Git runs the helper gittyup-editor, which passes the file to this server
// and waits until the dialog is closed.
class GitEditor : public QObject {
  Q_OBJECT

public:
  enum class Kind { Sequence, Message };

  // Call reply(true) when the file was edited, reply(false) to cancel.
  using Reply = std::function<void(bool)>;
  using Handler = std::function<void(Kind kind, const QString &file, Reply)>;

  GitEditor(Handler handler, QObject *parent = nullptr);
  ~GitEditor() override;

  // The helper next to the application (or the directory above, for tests),
  // or an empty string if it is missing.
  static QString helperPath();

  // Start listening. Returns an error message on failure.
  QString listen();

  // Make git use this editor for the todo list and commit messages.
  void setup(QProcessEnvironment &env) const;

  // True when the user canceled a dialog since the last call.
  bool takeCanceled();

private:
  QString command(const QString &kind) const;

  Handler mHandler;
  QLocalServer *mServer;
  QString mHelper;
  bool mCanceled = false;
};

#endif
