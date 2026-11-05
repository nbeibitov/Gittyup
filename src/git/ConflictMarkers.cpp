//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

#include "ConflictMarkers.h"

namespace git {

namespace {

const int kMarkerSize = 7;

// A marker is exactly seven characters, followed by a label or the line end.
// A longer run, such as a line of equal signs in a comment, is plain text.
bool isMarker(const QByteArray &line, char c) {
  if (line.size() < kMarkerSize)
    return false;

  for (int i = 0; i < kMarkerSize; ++i) {
    if (line.at(i) != c)
      return false;
  }

  if (line.size() == kMarkerSize)
    return true;

  char next = line.at(kMarkerSize);
  return next == ' ' || next == '\r' || next == '\n';
}

QByteArray label(const QByteArray &line) { return line.mid(kMarkerSize).trimmed(); }

} // namespace

QList<ConflictMarkers::Block> ConflictMarkers::parse(const QByteArray &text) {
  enum State { Outside, InOurs, InBase, InTheirs };

  QList<Block> blocks;
  Block block{};
  State state = Outside;

  int pos = 0;
  while (pos < text.size()) {
    int newline = text.indexOf('\n', pos);
    int next = newline < 0 ? text.size() : newline + 1;
    QByteArray line = text.mid(pos, next - pos);

    if (isMarker(line, '<')) {
      // Also drops a block that was never closed.
      block = Block{};
      block.begin = pos;
      block.ours.begin = next;
      block.oursLabel = label(line);
      state = InOurs;
    } else if (state == InOurs && isMarker(line, '|')) {
      block.ours.end = pos;
      state = InBase;
    } else if ((state == InOurs || state == InBase) && isMarker(line, '=')) {
      if (state == InOurs)
        block.ours.end = pos;
      block.theirs.begin = next;
      state = InTheirs;
    } else if (state == InTheirs && isMarker(line, '>')) {
      block.theirs.end = pos;
      block.theirsLabel = label(line);
      block.end = next;
      blocks.append(block);
      state = Outside;
    }

    pos = next;
  }

  return blocks;
}

QByteArray ConflictMarkers::resolution(const QByteArray &text,
                                       const Block &block, int sides) {
  QByteArray result;
  if (sides & Ours)
    result += text.mid(block.ours.begin, block.ours.end - block.ours.begin);
  if (sides & Theirs)
    result +=
        text.mid(block.theirs.begin, block.theirs.end - block.theirs.begin);
  return result;
}

ConflictMarkers::Version ConflictMarkers::version(const QByteArray &text,
                                                  const QList<Block> &blocks,
                                                  Side side) {
  Version version;
  int pos = 0;
  for (const Block &block : blocks) {
    version.text += text.mid(pos, block.begin - pos);

    int begin = version.text.size();
    version.text += resolution(text, block, side);
    version.blocks.append({begin, static_cast<int>(version.text.size())});

    pos = block.end;
  }

  version.text += text.mid(pos);
  return version;
}

} // namespace git
