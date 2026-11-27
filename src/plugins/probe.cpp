#include "deckhand/plugins/probe.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace deckhand::plugins {
namespace {
double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

ProbeResult probe_plugin(const PluginInfo& info, double sr, int block) {
  ProbeResult r;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    auto inst = open_plugin(info, sr, block, &err);
    r.open_ms = ms_since(t0);
    if (!inst) {
      r.error = "open failed: " + err;
      return r;
    }
    r.params = (int)inst->params().size();
    r.latency_seconds = inst->latency_seconds();
    r.tail_seconds = inst->tail_seconds();
    std::vector<float> l((std::size_t)block), rr((std::size_t)block), ol((std::size_t)block, 0), orr((std::size_t)block, 0);
    for (int i = 0; i < block; ++i) l[(std::size_t)i] = rr[(std::size_t)i] = 0.25f * (float)std::sin(2 * 3.14159265358979 * 440.0 * i / sr);
    const float* in[2] = {l.data(), rr.data()};
    float* out[2] = {ol.data(), orr.data()};
    const auto t1 = std::chrono::steady_clock::now();
    inst->process(in, out, block);
    r.render_ms = ms_since(t1);
    double peak = 0;
    for (int i = 0; i < block; ++i)
      for (int c = 0; c < 2; ++c) {
        const float v = out[c][i];
        if (!std::isfinite(v)) {
          r.error = "output not finite";
          return r;
        }
        peak = std::max(peak, (double)std::fabs(v));
      }
    r.out_peak = peak;
    if (inst->render_errors()) {
      r.error = "render failed";
      return r;
    }
    inst.reset();
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = std::string("exception: ") + e.what();
  } catch (...) {
    r.error = "exception";
  }
  return r;
}

#if !defined(_WIN32)
ProbeResult probe_out_of_process(const std::string& exe, const std::string& id, int timeout_ms, double sr, int block) {
  ProbeResult r;
  int fds[2];
  if (pipe(fds) != 0) {
    r.error = "pipe failed";
    return r;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  const std::string s_sr = std::to_string((long long)sr), s_bl = std::to_string(block);
  char* argv[] = {const_cast<char*>(exe.c_str()), const_cast<char*>(id.c_str()), const_cast<char*>(s_sr.c_str()),
                  const_cast<char*>(s_bl.c_str()), nullptr};
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    r.error = "cannot start probe helper";
    return r;
  }
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  std::string outp;
  const auto t0 = std::chrono::steady_clock::now();
  int status = 0;
  bool done = false;
  while (!done) {
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) outp.append(buf, (std::size_t)n);
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      done = true;
      while ((n = read(fds[0], buf, sizeof buf)) > 0) outp.append(buf, (std::size_t)n);
      break;
    }
    if (ms_since(t0) > timeout_ms) {
      kill(pid, SIGKILL);  // our own child, by exact pid
      waitpid(pid, &status, 0);
      close(fds[0]);
      r.error = "timed out";
      return r;
    }
    usleep(5000);
  }
  close(fds[0]);
  if (WIFSIGNALED(status)) {
    r.error = "crashed (signal " + std::to_string(WTERMSIG(status)) + ")";
    return r;
  }
  // Line: "ok <open_ms> <render_ms> <peak> <params> <latency> <tail>" or "fail <message>"
  if (outp.rfind("ok ", 0) == 0) {
    std::sscanf(outp.c_str(), "ok %lf %lf %lf %d %lf %lf", &r.open_ms, &r.render_ms, &r.out_peak, &r.params, &r.latency_seconds,
                &r.tail_seconds);
    r.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!r.ok) r.error = "helper exited abnormally";
  } else {
    r.error = outp.rfind("fail ", 0) == 0 ? outp.substr(5, outp.find('\n') == std::string::npos ? std::string::npos : outp.find('\n') - 5)
                                           : "no result from helper (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
  }
  return r;
}
#else
ProbeResult probe_out_of_process(const std::string&, const std::string&, int, double, int) {
  ProbeResult r;
  r.error = "out of process probe not supported on this platform";
  return r;
}
#endif

int probe_catalog(PluginCatalog& cat, const std::string& exe, int timeout_ms) {
  int blocked = 0;
  for (const auto& p : cat.usable()) {
    const ProbeResult r = probe_out_of_process(exe, p.id, timeout_ms);
    if (!r.ok) {
      cat.block(p.id, r.error);
      ++blocked;
    }
  }
  return blocked;
}

int probe_main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("fail usage: deckhand-plugin-probe <plugin id> [sample rate] [block]\n", stdout);
    return 2;
  }
  const double sr = argc > 2 ? std::atof(argv[2]) : 48000;
  const int block = argc > 3 ? std::atoi(argv[3]) : 512;
  const std::string id = argv[1];
  PluginInfo found;
  bool have = false;
  for (const auto& p : scan_installed())
    if (p.id == id) {
      found = p;
      have = true;
    }
  if (!have) {
    std::printf("fail unknown plug-in %s\n", id.c_str());
    return 3;
  }
  const ProbeResult r = probe_plugin(found, sr > 0 ? sr : 48000, block > 0 ? block : 512);
  if (!r.ok) {
    std::printf("fail %s\n", r.error.c_str());
    return 1;
  }
  std::printf("ok %.3f %.3f %.6f %d %.6f %.6f\n", r.open_ms, r.render_ms, r.out_peak, r.params, r.latency_seconds, r.tail_seconds);
  std::fflush(stdout);
  return 0;
}

}  // namespace deckhand::plugins
