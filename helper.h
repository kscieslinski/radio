#ifndef RADIO_HELPER_H
#define RADIO_HELPER_H

#include <cstdint>

union nuint64_t {
  uint64_t nuint64;
  uint32_t nuint32[2];
  char nuint8[8];
};


constexpr int STDIN = 0;
constexpr int STDOUT = 1;
constexpr uint64_t BUF_SIZE = 10000;
constexpr char NULL_TERMINATOR = '\0';
constexpr char EOL = '\n';
constexpr int DOTTED_ADDR_SIZE = 19;
constexpr int STATION_MAX_NAME_SIZE = 64;
constexpr int AUDIO_DATA = 16;


int64_t next(int64_t count, int64_t pos, int64_t total_elements_number);
int64_t prev(int64_t count, int64_t pos, int64_t total_elements_number);

#endif //RADIO_HELPER_H
