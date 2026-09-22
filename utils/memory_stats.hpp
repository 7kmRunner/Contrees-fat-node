#pragma once

#include <cstdint>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace contrees::memory_stats {

struct sample {
  std::uint64_t current_rss_bytes;
  std::uint64_t peak_rss_bytes;
};

// getrusage reports bytes on Darwin and KiB on Linux. Keep that platform
// difference contained here so callers always receive bytes.
inline std::uint64_t peak_rss_bytes() noexcept {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) { return 0; }

#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

inline std::uint64_t current_rss_bytes() noexcept {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const kern_return_t status = task_info(
      mach_task_self(), MACH_TASK_BASIC_INFO,
      reinterpret_cast<task_info_t>(&info), &count);
  if (status != KERN_SUCCESS) { return 0; }
  return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
  std::FILE* statm = std::fopen("/proc/self/statm", "r");
  if (statm == nullptr) { return 0; }

  unsigned long total_pages = 0;
  unsigned long resident_pages = 0;
  const int fields = std::fscanf(statm, "%lu %lu", &total_pages,
                                 &resident_pages);
  std::fclose(statm);
  if (fields != 2) { return 0; }

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) { return 0; }
  return static_cast<std::uint64_t>(resident_pages) *
         static_cast<std::uint64_t>(page_size);
#else
  return 0;
#endif
}

inline sample sample_now() noexcept {
  return {current_rss_bytes(), peak_rss_bytes()};
}

}  // namespace contrees::memory_stats
