//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef FILTER_H
#define FILTER_H

namespace git {

class Filter {
public:
  static void init();

  // Stop the long-running filters that this thread started. They keep the
  // working directory of their repository, which can't be removed on
  // Windows while they run. They start again when they are needed.
  static void stopProcesses();
};

} // namespace git

#endif
