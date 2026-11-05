//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#ifndef GIT_CONFLICTMARKERS_H
#define GIT_CONFLICTMARKERS_H

#include <QByteArray>
#include <QList>

namespace git {

// The conflict blocks that git leaves in the text of a file. All offsets are
// in bytes, so line endings and encodings stay untouched.
class ConflictMarkers {
public:
  enum Side { Ours = 1, Theirs = 2 };

  struct Range {
    int begin;
    int end;
  };

  struct Block {
    int begin; // start of the <<<<<<< line
    int end;   // past the >>>>>>> line
    Range ours;
    Range theirs;
    QByteArray oursLabel;
    QByteArray theirsLabel;
  };

  // The text with every block replaced by one side of it.
  struct Version {
    QByteArray text;
    QList<Range> blocks;
  };

  static QList<Block> parse(const QByteArray &text);

  // What replaces the block: any combination of sides, ours first.
  static QByteArray resolution(const QByteArray &text, const Block &block,
                               int sides);

  static Version version(const QByteArray &text, const QList<Block> &blocks,
                         Side side);
};

} // namespace git

#endif
