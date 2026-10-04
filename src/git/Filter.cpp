//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Filter.h"
#include "Command.h"
#include "Config.h"
#include "git2/errors.h"
#include "git2/filter.h"
#include "git2/repository.h"
#include "git2/sys/errors.h"
#include "git2/sys/filter.h"
#include <QCoreApplication>
#include <QMap>
#include <QPair>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <map>
#include <memory>

namespace git {

namespace {

QString kFilterFmt = "filter=%1";

struct FilterInfo {
  FilterInfo() : filter(GIT_FILTER_INIT) {}

  git_filter filter;

  QString clean;
  QString smudge;
  QString process; // a long-running filter, e.g. 'git-lfs filter-process'
  bool required = false;

  QByteArray name;
  QByteArray attributes;
};

QString quote(const QString &path) {
  QString escapedPath = path;
  // Ensure that the path is properly escaped to avoid shell injection
  // This is inspired by git's sq_quote_buf
  escapedPath.replace("'", "'\\''");
  return QString("'%1'").arg(escapedPath);
}

// The client side of the long-running filter protocol of git, see
// gitattributes(5). One process filters all files instead of one process
// for each file, which takes long for many files, e.g. with Git LFS.
class FilterProcess {
public:
  enum Result { Success, Error, Failed };

  ~FilterProcess() {
    // The filter exits at the end of its input.
    mProcess.closeWriteChannel();
    if (!mProcess.waitForFinished(2000)) {
      mProcess.kill();
      mProcess.waitForFinished(2000);
    }
  }

  bool start(const QString &command, const QString &workdir, QString *error) {
    QString bash = Command::bashPath();
    if (bash.isEmpty()) {
      *error = QObject::tr("bash was not found");
      return false;
    }

    mProcess.setWorkingDirectory(workdir);
    mProcess.start(bash, {"-c", command});
    if (!mProcess.waitForStarted(-1)) {
      *error = mProcess.errorString();
      return false;
    }

    // Handshake.
    QList<QByteArray> lines;
    if (!writePacket("git-filter-client\n") || !writePacket("version=2\n") ||
        !writeFlush() || !readList(lines) ||
        !lines.contains("git-filter-server") || !lines.contains("version=2")) {
      *error = failure(QObject::tr("the filter doesn't speak version 2"));
      return false;
    }

    if (!writePacket("capability=clean\n") ||
        !writePacket("capability=smudge\n") || !writeFlush() ||
        !readList(lines)) {
      *error = failure(QObject::tr("the filter has no capabilities"));
      return false;
    }

    for (const QByteArray &line : lines) {
      if (line.startsWith("capability="))
        mCapabilities.insert(line.mid(11));
    }

    return true;
  }

  bool supports(const QByteArray &command) const {
    return mCapabilities.contains(command);
  }

  Result filter(const QByteArray &command, const QByteArray &path,
                const QByteArray &input, QByteArray &output, QString *error) {
    if (!writePacket("command=" + command + "\n") ||
        !writePacket("pathname=" + path + "\n") || !writeFlush())
      return failed(error);

    for (qsizetype pos = 0; pos < input.size(); pos += kMaxPacketData) {
      if (!writePacket(input.mid(pos, kMaxPacketData), false))
        return failed(error);
    }

    QList<QByteArray> status;
    if (!writeFlush() || !readList(status))
      return failed(error);

    if (Result result = check(status, command, error); result != Success)
      return result;

    // The content, and a status list that may replace the first one.
    output.clear();
    for (;;) {
      QByteArray data;
      bool flush = false;
      if (!readPacket(data, flush))
        return failed(error);
      if (flush)
        break;
      output.append(data);
    }

    if (!readList(status))
      return failed(error);

    return check(status, command, error);
  }

private:
  // Bytes of data in a packet: 65520 minus the length in 4 hex digits.
  static const qsizetype kMaxPacketData = 65516;

  Result check(const QList<QByteArray> &status, const QByteArray &command,
               QString *error) {
    QByteArray value;
    for (const QByteArray &line : status) {
      if (line.startsWith("status="))
        value = line.mid(7);
    }

    if (value.isEmpty() || value == "success")
      return Success;

    // The filter refuses this command for all following files.
    if (value == "abort")
      mCapabilities.remove(command);

    *error = QObject::tr("the filter returned the status '%1'")
                 .arg(QString::fromUtf8(value));
    return Error;
  }

  Result failed(QString *error) {
    *error = failure(QObject::tr("the filter stopped"));
    return Failed;
  }

  QString failure(const QString &text) {
    QString output = QString::fromUtf8(mProcess.readAllStandardError());
    return output.trimmed().isEmpty() ? text : output.trimmed();
  }

  bool writePacket(const QByteArray &data, bool wait = true) {
    QByteArray size = QByteArray::number(data.size() + 4, 16);
    mProcess.write(size.rightJustified(4, '0') + data);
    return !wait || waitForWritten();
  }

  bool writeFlush() {
    mProcess.write("0000");
    return waitForWritten();
  }

  bool waitForWritten() {
    while (mProcess.bytesToWrite() > 0) {
      if (!mProcess.waitForBytesWritten(-1))
        return false;
    }
    return true;
  }

  bool read(qint64 size, QByteArray &data) {
    while (mProcess.bytesAvailable() < size) {
      if (!mProcess.waitForReadyRead(-1))
        return false;
    }
    data = mProcess.read(size);
    return true;
  }

  bool readPacket(QByteArray &data, bool &flush) {
    QByteArray header;
    if (!read(4, header))
      return false;

    bool ok = false;
    int size = header.toInt(&ok, 16);
    if (!ok || (size > 0 && size < 4))
      return false;

    flush = (size == 0);
    if (flush) {
      data.clear();
      return true;
    }

    return read(size - 4, data);
  }

  // Lines up to the next flush packet, without their line feeds.
  bool readList(QList<QByteArray> &lines) {
    lines.clear();
    for (;;) {
      QByteArray data;
      bool flush = false;
      if (!readPacket(data, flush))
        return false;
      if (flush)
        return true;
      if (data.endsWith('\n'))
        data.chop(1);
      lines.append(data);
    }
  }

  QProcess mProcess;
  QSet<QByteArray> mCapabilities;
};

// Long-running filters of the main thread stop after this time unused.
const int kIdleTimeout = 10000;

// A QProcess must be used by the thread that created it, so each thread
// keeps its own filter processes, for each working directory and filter.
using ProcessKey = QPair<QString, QByteArray>;
thread_local std::map<ProcessKey, std::unique_ptr<FilterProcess>> tProcesses;

FilterProcess *filterProcess(const FilterInfo *info, const QString &workdir,
                             QString *error) {
  ProcessKey key(workdir, info->name);
  auto it = tProcesses.find(key);
  if (it != tProcesses.end())
    return it->second.get();

  // Processes of other threads stop when their thread ends, e.g. when a
  // thread of the pool expires. The main thread stops them once they are
  // idle, and before the application is gone.
  QCoreApplication *app = QCoreApplication::instance();
  if (app && QThread::currentThread() == app->thread()) {
    static QTimer *idle = nullptr;
    if (!idle) {
      idle = new QTimer(app);
      idle->setSingleShot(true);
      QObject::connect(idle, &QTimer::timeout, [] { tProcesses.clear(); });
      QObject::connect(app, &QCoreApplication::aboutToQuit,
                       [] { tProcesses.clear(); });
    }
    idle->start(kIdleTimeout);
  }

  auto process = std::make_unique<FilterProcess>();
  if (!process->start(info->process, workdir, error))
    return nullptr;

  return tProcesses.emplace(key, std::move(process)).first->second.get();
}

// Run the long-running filter. Sets available to false if it can't run, or
// doesn't support the direction.
bool runProcess(const FilterInfo *info, git_filter_mode_t mode,
                const QString &workdir, const QByteArray &path,
                const QByteArray &input, QByteArray &output, QString *error,
                bool *available) {
  *available = false;
  QByteArray command = (mode == GIT_FILTER_SMUDGE) ? "smudge" : "clean";
  FilterProcess *process = filterProcess(info, workdir, error);
  if (!process || !process->supports(command))
    return false;

  *available = true;
  FilterProcess::Result result =
      process->filter(command, path, input, output, error);
  if (result == FilterProcess::Failed)
    tProcesses.erase(ProcessKey(workdir, info->name)); // start a new one

  return result == FilterProcess::Success;
}

// Run the clean or smudge command for one file.
bool runCommand(const FilterInfo *info, git_filter_mode_t mode,
                const QString &workdir, const QByteArray &path,
                const QByteArray &input, QByteArray &output, QString *error) {
  QString command = (mode == GIT_FILTER_SMUDGE) ? info->smudge : info->clean;
  if (command.isEmpty()) {
    if (error->isEmpty())
      *error = QObject::tr("the filter has no command for this direction");
    return false;
  }

  // Substitute path.
  command.replace("%f", quote(QString::fromUtf8(path)));

  QString bash = Command::bashPath();
  if (bash.isEmpty()) {
    *error = QObject::tr("bash was not found");
    return false;
  }

  QProcess process;
  process.setWorkingDirectory(workdir);
  process.start(bash, {"-c", command});
  if (!process.waitForStarted(-1)) {
    *error = process.errorString();
    return false;
  }

  process.write(input);
  process.closeWriteChannel();
  if (!process.waitForFinished(-1) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode()) {
    *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return false;
  }

  output = process.readAllStandardOutput();
  return true;
}

struct Stream {
  git_writestream parent; // must be the first. No pointer!
  git_writestream *next = nullptr;
  const FilterInfo *info = nullptr;
  git_filter_mode_t mode = GIT_FILTER_SMUDGE;
  QByteArray path;
  QString workdir;

  // The whole content, which is also needed to pass it through unchanged
  // when an optional filter fails.
  QByteArray input;
};

static void stream_free(git_writestream *stream) {
  auto *s = reinterpret_cast<struct Stream *>(stream);
  delete s;
}

static int stream_close(git_writestream *s) {
  struct Stream *stream = reinterpret_cast<struct Stream *>(s);
  const FilterInfo *info = stream->info;

  QByteArray output;
  QString error;
  bool available = false;
  bool filtered = false;
  if (!info->process.isEmpty())
    filtered = runProcess(info, stream->mode, stream->workdir, stream->path,
                          stream->input, output, &error, &available);

  // The single commands are used when there is no long-running filter.
  if (!available)
    filtered = runCommand(info, stream->mode, stream->workdir, stream->path,
                          stream->input, output, &error);

  if (!filtered) {
    if (info->required) {
      QString text = QObject::tr("The %1 filter failed for %2: %3")
                         .arg(QString::fromUtf8(info->name),
                              QString::fromUtf8(stream->path), error);
      git_error_set_str(GIT_ERROR_FILTER, text.toUtf8());
      stream->next->close(stream->next);
      return GIT_EUSER;
    }

    // Like git, an optional filter that fails leaves the content as it is.
    output = stream->input;
  }

  int result = stream->next->write(stream->next, output.constData(),
                                   static_cast<size_t>(output.size()));
  if (result == GIT_OK)
    result = stream->next->close(stream->next);
  return result;
}

static int stream_write(git_writestream *s, const char *buffer, size_t len) {
  struct Stream *stream = reinterpret_cast<struct Stream *>(s);
  stream->input.append(buffer, static_cast<qsizetype>(len));
  return 0;
}

// Called for every new stream
static int stream_init(git_writestream **out, git_filter *self, void **,
                       const git_filter_source *src, git_writestream *next) {
  const FilterInfo *info = reinterpret_cast<const FilterInfo *>(self);
  if (Command::bashPath().isEmpty())
    return info->required ? GIT_EUSER : GIT_PASSTHROUGH;

  struct Stream *stream = new Stream;
  stream->parent.write = stream_write;
  stream->parent.close = stream_close;
  stream->parent.free = stream_free;
  stream->next = next;
  stream->info = info;
  stream->mode = git_filter_source_mode(src);
  stream->path = git_filter_source_path(src);

  git_repository *repo = git_filter_source_repo(src);
  const char *workdir = repo ? git_repository_workdir(repo) : nullptr;
  stream->workdir = workdir ? QString::fromUtf8(workdir) : QString();

  *out = reinterpret_cast<git_writestream *>(stream);
  return 0;
}

} // namespace

static void shutdown(git_filter *) {}

void Filter::stopProcesses() { tProcesses.clear(); }

void Filter::init() {
  static QMap<QString, FilterInfo> filters;

  // Read global filters.
  Config config = Config::global();
  Config::Iterator it = config.glob("filter\\..*\\..*");
  while (git::Config::Entry entry = it.next()) {
    QString name = entry.name().section('.', 1, 1);
    QString key = entry.name().section('.', 2, 2);
    if (key == "clean") {
      filters[name].clean = entry.value<QString>();
    } else if (key == "smudge") {
      filters[name].smudge = entry.value<QString>();
    } else if (key == "process") {
      filters[name].process = entry.value<QString>();
    } else if (key == "required") {
      filters[name].required = entry.value<bool>();
    }
  }

  // Register filters.
  for (const QString &key : filters.keys()) {
    FilterInfo &info = filters[key];
    bool commands = !info.clean.isEmpty() && !info.smudge.isEmpty();
    if (!commands && info.process.isEmpty())
      continue;

    info.name = key.toUtf8();
    info.attributes = kFilterFmt.arg(key).toUtf8();

    info.filter.stream = stream_init;
    info.filter.shutdown = shutdown;
    info.filter.attributes = info.attributes.constData();
    // &info.filter can be used and reinterpret cast afterwards works, because
    // filter is the first member of FilterInfo. This is also proposed in
    // filter.h of libgit2
    git_filter_register(info.name.constData(), &info.filter,
                        GIT_FILTER_DRIVER_PRIORITY);
  }
}

} // namespace git
