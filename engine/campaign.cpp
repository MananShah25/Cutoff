#include "campaign.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "worker.hpp"

namespace cutoff::engine {

namespace {

struct Worker {
  pid_t pid = -1;
  int to_fd = -1;    // orchestrator -> worker
  int from_fd = -1;  // worker -> orchestrator
  std::uint64_t outstanding = 0;  // records still expected from this worker
  std::vector<char> inbox;        // bytes received but not yet forming a whole record
  bool open = false;
};

std::string describe_exit(int status) {
  if (WIFEXITED(status)) return "exit status " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return std::string("signal ") + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
  return "unknown status";
}

void close_worker_fds(Worker& w) {
  if (w.to_fd >= 0) close(w.to_fd);
  if (w.from_fd >= 0) close(w.from_fd);
  w.to_fd = w.from_fd = -1;
  w.open = false;
}

}  // namespace

bool OrderedDelivery::push(const ProgramResult& result) {
  if (stopped_) return true;
  const std::uint64_t id = result.rec.program_id;
  if (id < next_ || !waiting_.emplace(id, result).second) return false;
  for (auto it = waiting_.begin(); it != waiting_.end() && it->first == next_; it = waiting_.begin()) {
    const bool go_on = sink_.on_record(it->second);
    waiting_.erase(it);
    ++next_;
    if (!go_on) {
      stopped_ = true;
      waiting_.clear();
      break;
    }
  }
  return true;
}

CampaignStatus run_campaign(const CampaignConfig& cfg, RecordSink& sink, const CampaignControl& control,
                            const std::function<void(std::uint64_t)>& on_progress) {
  CampaignStatus status;
  const std::uint64_t first = control.first_id;
  const std::uint64_t total = cfg.programs;  // ids [first, total)
  status.next_id = first;
  if (first >= total) {  // nothing left to run
    status.ok = true;
    return status;
  }
  const std::uint64_t remaining = total - first;
  const std::uint32_t nworkers = static_cast<std::uint32_t>(std::min<std::uint64_t>(resolved_workers(cfg), (remaining + cfg.batch_size - 1) / cfg.batch_size));

  // A write to a dead worker must fail with EPIPE instead of killing the whole process.
  struct sigaction ignore {}, old_pipe {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  sigaction(SIGPIPE, &ignore, &old_pipe);

  std::vector<Worker> workers(nworkers);
  std::fflush(nullptr);  // do not let forked children replay our buffered output
  for (std::uint32_t i = 0; i < nworkers; ++i) {
    int down[2], up[2];
    if (pipe(down) != 0 || pipe(up) != 0) {
      status.error = std::string("cannot create pipes for workers: ") + std::strerror(errno) + ". Lower campaign.workers or raise the open-file limit (ulimit -n).";
      break;
    }
    const pid_t pid = fork();
    if (pid < 0) {
      status.error = std::string("cannot start worker process: ") + std::strerror(errno) + ". Lower campaign.workers.";
      close(down[0]); close(down[1]); close(up[0]); close(up[1]);
      break;
    }
    if (pid == 0) {  // worker: Ctrl-C belongs to the orchestrator, which winds the workers down itself
      signal(SIGINT, SIG_IGN);
      signal(SIGTERM, SIG_IGN);
      close(down[1]);
      close(up[0]);
      for (auto& other : workers) close_worker_fds(other);  // descriptors of earlier workers
      run_worker(cfg, down[0], up[1]);
    }
    close(down[0]);
    close(up[1]);
    workers[i] = {pid, down[1], up[0], 0, {}, true};
  }

  auto shutdown = [&](bool graceful) {
    for (auto& w : workers) {
      if (!w.open && w.pid < 0) continue;
      if (graceful && w.to_fd >= 0) {
        const Task stop{};
        write_full(w.to_fd, &stop, sizeof stop);
      } else if (w.pid > 0) {
        kill(w.pid, SIGKILL);
      }
      close_worker_fds(w);
      if (w.pid > 0) {
        int st = 0;
        waitpid(w.pid, &st, 0);
        if (graceful && status.error.empty() && !(WIFEXITED(st) && WEXITSTATUS(st) == 0))
          status.error = "a worker process ended with " + describe_exit(st) + " during shutdown.";
        w.pid = -1;
      }
    }
  };

  if (!status.error.empty()) {
    shutdown(false);
    sigaction(SIGPIPE, &old_pipe, nullptr);
    return status;
  }

  std::uint64_t next_to_assign = first;
  OrderedDelivery delivery(sink, first);
  auto last_progress = std::chrono::steady_clock::now();

  while (delivery.next_id() < total && !delivery.stopped_by_sink() && status.error.empty()) {
    if (control.interrupt != nullptr && *control.interrupt != 0) {
      status.interrupted = true;
      break;
    }
    // Keep every worker fed with up to two batches.
    for (auto& w : workers) {
      while (w.open && next_to_assign < total && w.outstanding <= cfg.batch_size) {
        const std::uint64_t count = std::min<std::uint64_t>(cfg.batch_size, total - next_to_assign);
        const Task task{next_to_assign, static_cast<std::uint32_t>(count), 0};
        if (!write_full(w.to_fd, &task, sizeof task)) {
          status.error = "lost contact with a worker process while sending it work.";
          break;
        }
        w.outstanding += count;
        next_to_assign += count;
      }
    }
    if (!status.error.empty()) break;

    std::vector<pollfd> fds;
    std::vector<std::size_t> owner;
    for (std::size_t i = 0; i < workers.size(); ++i)
      if (workers[i].open) { fds.push_back({workers[i].from_fd, POLLIN, 0}); owner.push_back(i); }
    if (fds.empty()) { status.error = "all worker processes have exited before the campaign finished."; break; }

    const int ready = poll(fds.data(), fds.size(), 200);
    if (ready < 0 && errno != EINTR) { status.error = std::string("poll failed: ") + std::strerror(errno); break; }

    for (std::size_t k = 0; k < fds.size() && ready > 0; ++k) {
      if ((fds[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;  // NOLINT(bugprone-signed-bitwise): POSIX poll flags are shorts
      Worker& w = workers[owner[k]];
      char buf[65536];
      const ssize_t n = read(w.from_fd, buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) continue;
        status.error = std::string("read from a worker failed: ") + std::strerror(errno);
        break;
      }
      if (n == 0) {  // EOF
        int st = 0;
        waitpid(w.pid, &st, 0);
        const pid_t dead = w.pid;
        w.pid = -1;
        close_worker_fds(w);
        if (w.outstanding > 0)
          status.error = "worker process " + std::to_string(dead) + " died with " + describe_exit(st) + " while " +
                         std::to_string(w.outstanding) + " programs were still assigned to it. Rerun with --workers 1 to reproduce, or check debug.crash_worker_at.";
        continue;
      }
      w.inbox.insert(w.inbox.end(), buf, buf + n);
      std::size_t off = 0;
      for (;;) {
        ProgramResult result;
        const std::size_t used = decode_frame(w.inbox.data() + off, w.inbox.size() - off, result);
        if (used == 0) break;
        off += used;
        if (result.rec.program_id >= total || !delivery.push(result)) {
          status.error = "a worker returned program id " + std::to_string(result.rec.program_id) + " which is out of range or was already delivered; this is a bug in Cutoff.";
          break;
        }
        if (w.outstanding > 0) --w.outstanding;
      }
      w.inbox.erase(w.inbox.begin(), w.inbox.begin() + static_cast<std::ptrdiff_t>(off));
      if (!status.error.empty()) break;
      if (delivery.stopped_by_sink()) break;
    }
    if (!status.error.empty()) break;

    const auto now = std::chrono::steady_clock::now();
    if (on_progress && now - last_progress >= std::chrono::milliseconds(200)) {
      on_progress(delivery.next_id());
      last_progress = now;
    }
  }

  status.next_id = delivery.next_id();
  status.stopped_by_sink = delivery.stopped_by_sink();
  const bool planned_end = status.error.empty() && !status.interrupted && (delivery.next_id() >= total || delivery.stopped_by_sink());
  shutdown(status.error.empty());
  sigaction(SIGPIPE, &old_pipe, nullptr);
  if (planned_end && on_progress) on_progress(delivery.next_id());
  status.ok = planned_end;
  return status;
}

}  // namespace cutoff::engine
