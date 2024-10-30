#pragma once
#if defined(__GNUC__)
#include <sys/resource.h>
#include <sys/time.h>
#endif

#if defined(WIN32)
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

// Link with Psapi.lib
#pragma comment(lib, "Psapi.lib")
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>

#if __has_include("ylt/coro_io/coro_io.hpp")
#include "ylt/coro_io/coro_io.hpp"
#include "ylt/coro_io/io_context_pool.hpp"
#include "ylt/metric/counter.hpp"
#include "ylt/metric/gauge.hpp"
#include "ylt/metric/metric.hpp"
#include "ylt/metric/metric_manager.hpp"
#else
#include "cinatra/ylt/coro_io/coro_io.hpp"
#include "cinatra/ylt/coro_io/io_context_pool.hpp"
#include "cinatra/ylt/metric/counter.hpp"
#include "cinatra/ylt/metric/gauge.hpp"
#include "cinatra/ylt/metric/metric.hpp"
#include "cinatra/ylt/metric/metric_manager.hpp"
#endif

// modified based on: brpc/src/bvar/default_variables.cpp

namespace ylt::metric {
namespace detail {

#if defined(__APPLE__)
#include <stdio.h>

inline int read_command_output_through_popen(std::ostream& os,
                                             const char* cmd) {
  FILE* pipe = popen(cmd, "r");
  if (pipe == NULL) {
    return -1;
  }
  char buffer[1024];
  for (;;) {
    size_t nr = fread(buffer, 1, sizeof(buffer), pipe);
    if (nr != 0) {
      os.write(buffer, nr);
    }
    if (nr != sizeof(buffer)) {
      if (feof(pipe)) {
        break;
      }
      else if (ferror(pipe)) {
        break;
      }
      // retry;
    }
  }

  const int wstatus = pclose(pipe);

  if (wstatus < 0) {
    return wstatus;
  }
  if (WIFEXITED(wstatus)) {
    return WEXITSTATUS(wstatus);
  }
  if (WIFSIGNALED(wstatus)) {
    os << "Child process was killed by signal " << WTERMSIG(wstatus);
  }
  errno = ECHILD;
  return -1;
}
#endif

#if defined(WIN32)
typedef struct timeval {
  long tv_sec;
  long tv_usec;
} timeval;

inline int gettimeofday(struct timeval* tp, struct timezone* tzp) {
  // Note: some broken versions only have 8 trailing zero's, the correct epoch
  // has 9 trailing zero's This magic number is the number of 100 nanosecond
  // intervals since January 1, 1601 (UTC) until 00:00:00 January 1, 1970
  static const uint64_t epoch = ((uint64_t)116444736000000000ULL);

  SYSTEMTIME system_time;
  FILETIME file_time;
  uint64_t time;

  GetSystemTime(&system_time);
  SystemTimeToFileTime(&system_time, &file_time);
  time = ((uint64_t)file_time.dwLowDateTime);
  time += ((uint64_t)file_time.dwHighDateTime) << 32;

  tp->tv_sec = (long)((time - epoch) / 10000000L);
  tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
  return 0;
}

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

struct rusage {
  struct timeval ru_utime; /* user time used */
  struct timeval ru_stime; /* system time used */
};

inline int getrusage(int who, struct rusage* rusage) {
  FILETIME starttime;
  FILETIME exittime;
  FILETIME kerneltime;
  FILETIME usertime;
  ULARGE_INTEGER li;

  if (who != RUSAGE_SELF) {
    /* Only RUSAGE_SELF is supported in this implementation for now */
    errno = EINVAL;
    return -1;
  }

  if (rusage == (struct rusage*)NULL) {
    errno = EFAULT;
    return -1;
  }
  memset(rusage, 0, sizeof(struct rusage));
  if (GetProcessTimes(GetCurrentProcess(), &starttime, &exittime, &kerneltime,
                      &usertime) == 0) {
    return -1;
  }

  /* Convert FILETIMEs (0.1 us) to struct timeval */
  memcpy(&li, &kerneltime, sizeof(FILETIME));
  li.QuadPart /= 10L; /* Convert to microseconds */
  rusage->ru_stime.tv_sec = li.QuadPart / 1000000L;
  rusage->ru_stime.tv_usec = li.QuadPart % 1000000L;

  memcpy(&li, &usertime, sizeof(FILETIME));
  li.QuadPart /= 10L; /* Convert to microseconds */
  rusage->ru_utime.tv_sec = li.QuadPart / 1000000L;
  rusage->ru_utime.tv_usec = li.QuadPart % 1000000L;

  return 0;
}

inline SIZE_T get_shared_memory_size(HANDLE h_process) {
  MEMORY_BASIC_INFORMATION mbi;
  SIZE_T base_address = 0;
  SIZE_T shared_memory_size = 0;

  while (VirtualQueryEx(h_process, (LPCVOID)base_address, &mbi, sizeof(mbi))) {
    if (mbi.State == MEM_COMMIT) {
      if (mbi.Type == MEM_MAPPED || mbi.Type == MEM_IMAGE) {
        shared_memory_size += mbi.RegionSize;
      }
    }
    base_address = (SIZE_T)mbi.BaseAddress + mbi.RegionSize;
  }

  return shared_memory_size;
}

inline DWORD getppid() {
  HANDLE h_snapshot;
  PROCESSENTRY32 pe32;
  DWORD ppid = 0, pid = GetCurrentProcessId();

  h_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  try {
    if (h_snapshot == INVALID_HANDLE_VALUE)
      return ppid;

    ZeroMemory(&pe32, sizeof(pe32));
    pe32.dwSize = sizeof(pe32);
    if (!Process32First(h_snapshot, &pe32))
      return ppid;

    do {
      if (pe32.th32ProcessID == pid) {
        ppid = pe32.th32ParentProcessID;
        break;
      }
    } while (Process32Next(h_snapshot, &pe32));

  } catch (...) {
    if (h_snapshot != INVALID_HANDLE_VALUE)
      CloseHandle(h_snapshot);
  }

  if (h_snapshot != INVALID_HANDLE_VALUE)
    CloseHandle(h_snapshot);

  return ppid;
}

inline DWORD get_thread_number(DWORD processId) {
  DWORD thread_count = 0;
  HANDLE snapshot_handle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

  if (snapshot_handle == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to create snapshot. Error code: " << GetLastError()
              << std::endl;
    return 0;
  }

  THREADENTRY32 threadEntry;
  threadEntry.dwSize = sizeof(THREADENTRY32);

  if (Thread32First(snapshot_handle, &threadEntry)) {
    do {
      if (threadEntry.th32OwnerProcessID == processId) {
        ++thread_count;
      }
    } while (Thread32Next(snapshot_handle, &threadEntry));
  }
  else {
    std::cerr << "Failed to retrieve thread information. Error code: "
              << GetLastError() << std::endl;
  }

  CloseHandle(snapshot_handle);
  return thread_count;
}

inline DWORD get_process_group(HANDLE process_handle) {
  DWORD_PTR process_affinity_mask;
  DWORD_PTR system_affinity_mask;

  if (GetProcessAffinityMask(process_handle, &process_affinity_mask,
                             &system_affinity_mask)) {
    // Output the processor group information
    // Process Affinity Mask
    DWORD grop_id = process_affinity_mask;
    return grop_id;
  }
  else {
    std::cerr << "Failed to get process affinity mask. Error code: "
              << GetLastError() << std::endl;
    return 0;
  }
}
#endif

inline int64_t last_time_us = 0;
inline int64_t last_sys_time_us = 0;
inline int64_t last_user_time_us = 0;

inline int64_t gettimeofday_us() {
  timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec * 1000000L + now.tv_usec;
}

inline int64_t timeval_to_microseconds(const timeval& tv) {
  return tv.tv_sec * 1000000L + tv.tv_usec;
}

inline void stat_cpu() {
  static auto process_cpu_usage =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_cpu_usage");
  static auto process_cpu_usage_system =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_cpu_usage_system");
  static auto process_cpu_usage_user =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_cpu_usage_user");

  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  int64_t utime = timeval_to_microseconds(usage.ru_utime);
  int64_t stime = timeval_to_microseconds(usage.ru_stime);
  int64_t time_total = utime + stime;
  int64_t now = gettimeofday_us();
  if (last_time_us == 0) {
    last_time_us = now;
    last_sys_time_us = stime;
    last_user_time_us = utime;
    return;
  }

  int64_t elapsed = now - last_time_us;
  if (elapsed == 0) {
    return;
  }

  double cpu_usage =
      double(time_total - (last_sys_time_us + last_user_time_us)) /
      (now - last_time_us);
  double sys_cpu_usage =
      double(stime - last_sys_time_us) / (now - last_time_us);
  double usr_cpu_usage =
      double(utime - last_user_time_us) / (now - last_time_us);
  process_cpu_usage->update(cpu_usage);
  process_cpu_usage_system->update(sys_cpu_usage);
  process_cpu_usage_user->update(usr_cpu_usage);

  last_time_us = now;
  last_sys_time_us = stime;
  last_user_time_us = utime;
}

inline void stat_memory() {
  static auto process_memory_virtual =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_memory_virtual");
  static auto process_memory_resident =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_memory_resident");
  static auto process_memory_shared =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_memory_shared");
  long virtual_size = 0;
  long resident = 0;
  long share = 0;
#if defined(__GNUC__)
  static long page_size = sysconf(_SC_PAGE_SIZE);
#endif

#if defined(__APPLE__)
  static pid_t pid = getpid();
  static int64_t pagesize = getpagesize();
  std::ostringstream oss;
  char cmdbuf[128];
  snprintf(cmdbuf, sizeof(cmdbuf), "ps -p %ld -o rss=,vsz=", (long)pid);
  if (read_command_output_through_popen(oss, cmdbuf) != 0) {
    return;
  }
  const std::string& result = oss.str();
  if (sscanf(result.c_str(), "%ld %ld", &resident, &virtual_size) != 2) {
    return;
  }
#else
  std::ifstream file("/proc/self/statm");
  if (!file) {
    return;
  }

  file >> virtual_size >> resident >> share;
#endif

#if defined(WIN32)
  DWORD current_process = GetCurrentProcessId();
  // open process
  HANDLE h_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, current_process);
  if (h_process == NULL) {
    virtual_size = 0;
    resident = 0;
    share = 0;
  }
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(h_process, &pmc, sizeof(pmc))) {
    virtual_size = pmc.PagefileUsage;
    resident = pmc.WorkingSetSize;
  }
  share = get_shared_memory_size(h_process);

  CloseHandle(h_process);

  process_memory_virtual->update(virtual_size);
  process_memory_resident->update(resident);
  process_memory_shared->update(share);
#else
  process_memory_virtual->update(virtual_size * page_size);
  process_memory_resident->update(resident * page_size);
  process_memory_shared->update(share * page_size);
#endif
}

struct ProcIO {
  size_t rchar;
  size_t wchar;
  size_t syscr;
  size_t syscw;
  size_t read_bytes;
  size_t write_bytes;
  size_t cancelled_write_bytes;
};

inline void stat_io() {
  static auto process_io_read_bytes_second =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_io_read_bytes_second");
  static auto process_io_write_bytes_second =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_io_write_bytes_second");
  static auto process_io_read_second =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_io_read_second");
  static auto process_io_write_second =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_io_write_second");

  ProcIO s{};
#if defined(__GUNC__)
  auto stream_file =
      std::shared_ptr<FILE>(fopen("/proc/self/io", "r"), [](FILE* ptr) {
        fclose(ptr);
      });
  if (stream_file == nullptr) {
    return;
  }

  if (fscanf(stream_file.get(),
             "%*s %lu %*s %lu %*s %lu %*s %lu %*s %lu %*s %lu %*s %lu",
             &s.rchar, &s.wchar, &s.syscr, &s.syscw, &s.read_bytes,
             &s.write_bytes, &s.cancelled_write_bytes) != 7) {
    return;
  }
#endif

#if defined(WIN32)
  DWORD current_process_id = GetCurrentProcessId();
  // open process
  HANDLE h_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, current_process_id);
  if (h_process == NULL) {
    s.rchar = 0;
    s.wchar = 0;
    s.syscr = 0;
    s.syscw = 0;
  }
  else {
    IO_COUNTERS io_counters = {0};
    if (GetProcessIoCounters(h_process, &io_counters)) {
      s.rchar = io_counters.ReadOperationCount;
      s.wchar = io_counters.WriteOperationCount;
      s.syscr = io_counters.ReadOperationCount;
      s.syscw = io_counters.WriteOperationCount;
    }
  }

  CloseHandle(h_process);
#endif

  process_io_read_bytes_second->update(s.rchar);
  process_io_write_bytes_second->update(s.wchar);
  process_io_read_second->update(s.syscr);
  process_io_write_second->update(s.syscw);
}

inline void stat_avg_load() {
  static auto system_loadavg_1m =
      system_metric_manager::instance().get_metric_static<gauge_d>(
          "ylt_system_loadavg_1m");
  static auto system_loadavg_5m =
      system_metric_manager::instance().get_metric_static<gauge_d>(
          "ylt_system_loadavg_5m");
  static auto system_loadavg_15m =
      system_metric_manager::instance().get_metric_static<gauge_d>(
          "ylt_system_loadavg_15m");

  double loadavg_1m = 0;
  double loadavg_5m = 0;
  double loadavg_15m = 0;

#if defined(__APPLE__)
  std::ostringstream oss;
  if (read_command_output_through_popen(oss, "sysctl -n vm.loadavg") != 0) {
    return;
  }
  const std::string& result = oss.str();
  if (sscanf(result.c_str(), "{ %lf %lf %lf }", &loadavg_1m, &loadavg_5m,
             &loadavg_15m) != 3) {
    return;
  }
#else
  std::ifstream file("/proc/loadavg");
  if (!file) {
    return;
  }

  file >> loadavg_1m >> loadavg_5m >> loadavg_15m;
#endif

  system_loadavg_1m->update(loadavg_1m);
  system_loadavg_5m->update(loadavg_5m);
  system_loadavg_15m->update(loadavg_15m);
}

struct ProcStat {
  int pid;
  // std::string comm;
  char state;
  int ppid;
  int pgrp;
  int session;
  int tty_nr;
  int tpgid;
  unsigned flags;
  unsigned long minflt;
  unsigned long cminflt;
  unsigned long majflt;
  unsigned long cmajflt;
  unsigned long utime;
  unsigned long stime;
  unsigned long cutime;
  unsigned long cstime;
  long priority;
  long nice;
  long num_threads;
};

inline void process_status() {
  static auto process_uptime =
      system_metric_manager::instance().get_metric_static<counter_t>(
          "ylt_process_uptime");
  static auto process_priority =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_process_priority");
  static auto pid =
      system_metric_manager::instance().get_metric_static<gauge_t>("ylt_pid");
  static auto ppid =
      system_metric_manager::instance().get_metric_static<gauge_t>("ylt_ppid");
  static auto pgrp =
      system_metric_manager::instance().get_metric_static<gauge_t>("ylt_pgrp");
  static auto thread_count =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_thread_count");

  ProcStat stat{};
#if defined(__linux__)
  auto stream_file =
      std::shared_ptr<FILE>(fopen("/proc/self/stat", "r"), [](FILE* ptr) {
        fclose(ptr);
      });
  if (stream_file == nullptr) {
    return;
  }

  if (fscanf(stream_file.get(),
             "%d %*s %c "
             "%d %d %d %d %d "
             "%u %lu %lu %lu "
             "%lu %lu %lu %lu %lu "
             "%ld %ld %ld",
             &stat.pid, &stat.state, &stat.ppid, &stat.pgrp, &stat.session,
             &stat.tty_nr, &stat.tpgid, &stat.flags, &stat.minflt,
             &stat.cminflt, &stat.majflt, &stat.cmajflt, &stat.utime,
             &stat.stime, &stat.cutime, &stat.cstime, &stat.priority,
             &stat.nice, &stat.num_threads) != 19) {
    return;
  }
#elif defined(__APPLE__)
  static pid_t proc_id = getpid();
  std::ostringstream oss;
  char cmdbuf[128];
  snprintf(cmdbuf, sizeof(cmdbuf),
           "ps -p %ld -o pid,ppid,pgid,sess"
           ",tpgid,flags,pri,nice | tail -n1",
           (long)proc_id);
  if (read_command_output_through_popen(oss, cmdbuf) != 0) {
    return;
  }
  const std::string& result = oss.str();
  if (sscanf(result.c_str(),
             "%d %d %d %d"
             "%d %u %ld %ld",
             &stat.pid, &stat.ppid, &stat.pgrp, &stat.session, &stat.tpgid,
             &stat.flags, &stat.priority, &stat.nice) != 8) {
    return;
  }
#elif defined(WIN32)
  stat.pid = GetCurrentProcessId();
  stat.ppid = getppid();

  HANDLE h_process =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, stat.pid);
  stat.priority = GetPriorityClass(h_process);
  stat.num_threads = get_thread_number(stat.pid);
  stat.pgrp = get_process_group(h_process);
  CloseHandle(h_process);
#endif
  process_uptime->inc();
  process_priority->update(stat.priority);
  pid->update(stat.pid);
  ppid->update(stat.ppid);
  pgrp->update(stat.pgrp);
  thread_count->update(stat.num_threads);
}

inline void stat_metric() {
  static auto user_metric_count =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_user_metric_count");
  user_metric_count->update(metric::metric_t::g_user_metric_count);

  static auto user_metric_label_count =
      system_metric_manager::instance().get_metric_static<gauge_t>(
          "ylt_user_metric_labels");
  user_metric_label_count->update(
      dynamic_metric::g_user_metric_label_count->value());
}

inline void ylt_stat() {
  stat_cpu();
  stat_memory();
  stat_io();
  stat_avg_load();
  process_status();
  stat_metric();
}

inline void start_stat(std::weak_ptr<coro_io::period_timer> weak) {
  auto timer = weak.lock();
  if (timer == nullptr) {
    return;
  }

  timer->expires_after(std::chrono::seconds(1));
  timer->async_wait([timer](std::error_code ec) {
    if (ec) {
      return;
    }

    ylt_stat();
    start_stat(timer);
  });
}
}  // namespace detail

inline bool start_system_metric() {
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_cpu_usage", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_cpu_usage_system", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_cpu_usage_user", "");

  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_memory_virtual", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_memory_resident", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_memory_shared", "");

  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_uptime", "");
  system_metric_manager::instance().create_metric_static<gauge_t>("ylt_pid",
                                                                  "");
  system_metric_manager::instance().create_metric_static<gauge_t>("ylt_ppid",
                                                                  "");
  system_metric_manager::instance().create_metric_static<gauge_t>("ylt_pgrp",
                                                                  "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_thread_count", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_priority", "");

  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_user_metric_count", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_user_metric_labels", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_summary_failed_count", "");

  system_metric_manager::instance().create_metric_static<gauge_d>(
      "ylt_system_loadavg_1m", "");
  system_metric_manager::instance().create_metric_static<gauge_d>(
      "ylt_system_loadavg_5m", "");
  system_metric_manager::instance().create_metric_static<gauge_d>(
      "ylt_system_loadavg_15m", "");

  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_io_read_bytes_second", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_io_write_bytes_second", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_io_read_second", "");
  system_metric_manager::instance().create_metric_static<gauge_t>(
      "ylt_process_io_write_second", "");

  static auto exucutor = coro_io::create_io_context_pool(1);
  auto timer =
      std::make_shared<coro_io::period_timer>(exucutor->get_executor());
  detail::start_stat(timer);

  return true;
}
}  // namespace ylt::metric