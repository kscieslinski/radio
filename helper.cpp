#include <sys/time.h>
#include "helper.h"

int64_t next(int64_t count, int64_t pos, int64_t total_elements_number) {
  return (pos + count) % total_elements_number;
}

int64_t prev(int64_t count, int64_t pos, int64_t total_elements_number) {
  int64_t res_pos;

  res_pos = (pos - count) % total_elements_number;
  if (res_pos < 0) {
    res_pos += total_elements_number;
  }

  return res_pos;
}

uint64_t get_microseconds() {
  struct timeval tv{};
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(1000000 * tv.tv_sec + tv.tv_usec);
}