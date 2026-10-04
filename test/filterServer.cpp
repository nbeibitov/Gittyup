//
//          Copyright (c) 2026, Gittyup Community
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Beybitov Nurzhan
//

// A long-running filter for the tests, see gitattributes(5). Smudging adds
// "S:" to the content and cleaning removes it. Paths that contain "fail"
// fail. Starts and requests are appended to the file in
// $GITTYUP_FILTER_LOG.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void log(const std::string &line) {
  const char *path = std::getenv("GITTYUP_FILTER_LOG");
  if (!path)
    return;
  if (FILE *file = std::fopen(path, "ab")) {
    std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
  }
}

bool readExact(char *data, size_t size) {
  return std::fread(data, 1, size, stdin) == size;
}

// Returns false at the end of the input. flush is set for a flush packet.
bool readPacket(std::string &data, bool &flush) {
  char header[5] = {0};
  if (!readExact(header, 4))
    return false;

  size_t size = std::strtoul(header, nullptr, 16);
  flush = (size == 0);
  data.clear();
  if (flush)
    return true;

  data.resize(size - 4);
  return readExact(&data[0], size - 4);
}

bool readList(std::vector<std::string> &lines) {
  lines.clear();
  for (;;) {
    std::string data;
    bool flush = false;
    if (!readPacket(data, flush))
      return false;
    if (flush)
      return true;
    if (!data.empty() && data.back() == '\n')
      data.pop_back();
    lines.push_back(data);
  }
}

void writePacket(const std::string &data) {
  std::printf("%04zx", data.size() + 4);
  std::fwrite(data.data(), 1, data.size(), stdout);
}

void writeFlush() {
  std::fwrite("0000", 1, 4, stdout);
  std::fflush(stdout);
}

void writeContent(const std::string &content) {
  for (size_t pos = 0; pos < content.size(); pos += 65516)
    writePacket(content.substr(pos, 65516));
  writeFlush();
}

} // namespace

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  log("start");

  std::vector<std::string> lines;
  if (!readList(lines))
    return 1;
  writePacket("git-filter-server\n");
  writePacket("version=2\n");
  writeFlush();

  if (!readList(lines))
    return 1;
  writePacket("capability=clean\n");
  writePacket("capability=smudge\n");
  writeFlush();

  while (readList(lines)) {
    std::string command, path;
    for (const std::string &line : lines) {
      if (line.rfind("command=", 0) == 0)
        command = line.substr(8);
      else if (line.rfind("pathname=", 0) == 0)
        path = line.substr(9);
    }

    std::string content, data;
    bool flush = false;
    while (readPacket(data, flush) && !flush)
      content += data;

    log(command + " " + path);

    if (path.find("fail") != std::string::npos) {
      writePacket("status=error\n");
      writeFlush();
      continue;
    }

    if (command == "smudge")
      content = "S:" + content;
    else if (content.rfind("S:", 0) == 0)
      content = content.substr(2);

    writePacket("status=success\n");
    writeFlush();
    writeContent(content);
    writeFlush(); // keep the status
  }

  return 0;
}
