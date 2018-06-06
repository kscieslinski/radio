
#include <cstdint>
#include <vector>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <string>
#include <cstdio>
#include <arpa/inet.h>
#include <cassert>
#include <zconf.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include "helper.h"


constexpr short LOOKUP_ORD = 1;
constexpr short REXMIT_ORD = 2;
constexpr short NO_ORD = 3;
constexpr short TRANSMITTER_NAP = 1;

struct package_t {
  uint64_t fbyte;
  bool retransmit;
  char* data;
};


/* sender run parameters */
uint16_t DATA_PORT;
uint16_t CTRL_PORT;
uint64_t PSIZE;
uint64_t FSIZE;
uint64_t RTIME;
char* MCAST_ADDR;
char* SNAME;

uint64_t MAX_PACKAGES_NO;
nuint64_t SESSION_ID;

/* shared */
std::vector<package_t> packages;
nuint64_t read_bytes;
bool data_left;

int tsock;
std::mutex tsock_mut;

/* controler */
int csock;

/* transmitter */
sockaddr_in multicast_addr;


int64_t package_pos(uint64_t fbyte) {
  int64_t head_pos, pos, package_delay;
  uint64_t tmp_read_bytes;

  tmp_read_bytes = read_bytes.nuint64;
  head_pos = (tmp_read_bytes / PSIZE) % MAX_PACKAGES_NO;
  package_delay = (tmp_read_bytes - fbyte) / PSIZE;

  pos = prev(package_delay, head_pos, MAX_PACKAGES_NO);
  assert(pos >= 0 && pos < MAX_PACKAGES_NO);

  return pos;
}

void send_package(package_t& package) {
  int flags;
  socklen_t multicast_address_len;
  ssize_t snd_len;

  flags = 0;
  multicast_address_len = sizeof(multicast_addr);

  //if (time(nullptr) % 3 == 0) return; /* simulating data lose */

  snd_len = sendto(tsock, package.data, PSIZE + AUDIO_DATA + 1, flags,
                   (const struct sockaddr *) &multicast_addr, multicast_address_len);
  if (snd_len != PSIZE + AUDIO_DATA + 1) {
    fprintf(stderr, "sendto multicaster");
  }

  package.retransmit = false;
}

void init() {
  data_left = true;
  SNAME = const_cast<char *>("Radio Pruszkow");
  MCAST_ADDR = const_cast<char *>("239.10.11.12");
  DATA_PORT = 27075;
  CTRL_PORT = 37075;
  RTIME = 250;
  FSIZE = 131072; //50;
  PSIZE = 512;
  MAX_PACKAGES_NO = FSIZE / PSIZE;
  SESSION_ID.nuint64 = static_cast<uint64_t>(time(nullptr));
  read_bytes.nuint64 = 0;

  packages.resize(MAX_PACKAGES_NO);
  for (int i = 0; i < MAX_PACKAGES_NO; ++i) {
    packages[i].data = new char[PSIZE + AUDIO_DATA + 1];
    memset(packages[i].data, 0, PSIZE + AUDIO_DATA + 1);
  }
  SESSION_ID.nuint32[0] = htonl(SESSION_ID.nuint32[0]);
  SESSION_ID.nuint32[1] = htonl(SESSION_ID.nuint32[1]);
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  CONTROLER                                                         *
 *--------------------------------------------------------------------------------------------------------------------*/
void cinit() {
  int broadcast_val, res;
  sockaddr_in self_address{};

  self_address.sin_family = AF_INET;
  self_address.sin_addr.s_addr = htonl(INADDR_ANY);
  self_address.sin_port = htons(CTRL_PORT);

  csock = socket(AF_INET, SOCK_DGRAM, 0);
  if (csock < 0) {
    fprintf(stderr, "ctrl_sock");
  }

  broadcast_val = 1;
  res = setsockopt(csock, SOL_SOCKET, SO_BROADCAST, &broadcast_val, sizeof(broadcast_val));
  if (res < 0) {
    fprintf(stderr, "setsockopt SO_BROADCAST");
  }

  res = bind(csock, (struct sockaddr*) &self_address, (socklen_t) sizeof(self_address));
  if (res < 0) {
    fprintf(stderr, "bind crtl_sock");
  }

}

int cread_order(sockaddr_in& rec_addr, char* buff) {
  int flags;
  ssize_t rec_bytes;
  socklen_t rec_addr_len;

  flags = 0;
  rec_addr_len = sizeof(rec_addr);

  rec_bytes = recvfrom(csock, buff, BUF_SIZE, flags, (struct sockaddr *) &rec_addr, &rec_addr_len);
  if (rec_bytes < 0) {
    fprintf(stderr, "error on datagram from ctrl sock");
  }

  buff[rec_bytes] = NULL_TERMINATOR;

  printf("CTRL_SOCK: %.*s from %s\n", (int) rec_bytes, buff, inet_ntoa(rec_addr.sin_addr));

  if (strcmp(buff, "LOOKUP") == 0) {
    return LOOKUP_ORD;
  } else if (strncmp(buff, "LOUDER_PLEASE", strlen("LOUDER_PLEASE")) == 0) {
    return REXMIT_ORD;
  } else {
    return NO_ORD;
  }
}

void cperform_lookup_ord(struct sockaddr_in& rec_addr) {
  char reply[BUF_SIZE + 1];
  int flags;
  ssize_t snd_len;
  socklen_t rec_addr_len;

  flags = 0;
  rec_addr_len = sizeof(rec_addr);
  sprintf(reply, "%s %d %s", MCAST_ADDR, DATA_PORT, SNAME);

  snd_len = sendto(csock, reply, strlen(reply), flags, (const struct sockaddr *) &rec_addr, rec_addr_len);
  if (snd_len < 0) {
    fprintf(stderr, "sendto lookup");
  }
}

void cmark_retransmition_request(uint64_t missing_package_fbyte) {
  int64_t missing_package_pos;

  missing_package_pos = package_pos(missing_package_fbyte);

  if (packages[missing_package_pos].fbyte == missing_package_fbyte) {
    packages[missing_package_pos].retransmit = true;
  }
}

void cperform_rexmit_ord(char* buffer) {
  uint64_t package_to_retransmit;
  const char* coma_delimiter;
  char* packages_list;
  char* token;
  char buf_copy[BUF_SIZE + 1];

  coma_delimiter = ",";

  memcpy(buf_copy, buffer, BUF_SIZE + 1);
  packages_list = buf_copy + strlen("LOUDER_PLEASE") + 1;

  token = strtok(packages_list, coma_delimiter);
  while (token) {
    package_to_retransmit = (uint64_t) atoll(token);
    cmark_retransmition_request(package_to_retransmit);
    token = strtok(nullptr, coma_delimiter);
  }
}

void cperform_order(short order, char* buff, sockaddr_in& rec_addr) {
  switch (order) {
    case LOOKUP_ORD:
      cperform_lookup_ord(rec_addr);
      break;

    case REXMIT_ORD:
      cperform_rexmit_ord(buff);
      break;

    default:
      break;
  }
}

void controler() {
  sockaddr_in rec_addr{};
  int order;
  char buff[BUF_SIZE + 1];
  cinit();

  while (true) {
    memset(buff, 0, BUF_SIZE + 1);
    order = cread_order(rec_addr, buff);
    cperform_order(order, buff, rec_addr);
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  RETRANSMITTER                                                     *
 *--------------------------------------------------------------------------------------------------------------------*/
void retransmit() {
  int64_t ptr, head;

  tsock_mut.lock();

  head = package_pos(read_bytes.nuint64);
  ptr = next(1, head, MAX_PACKAGES_NO);

  while (ptr != head) {
    if (packages[ptr].retransmit) {
      send_package(packages[ptr]);
    }
    ptr = next(1, ptr, MAX_PACKAGES_NO);
  }

  tsock_mut.unlock();
}

void retransmitter() {
  while (data_left) {
    retransmit();
    usleep(static_cast<__useconds_t>(RTIME));
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  TRANSMITTER                                                       *
 *--------------------------------------------------------------------------------------------------------------------*/
void tinit() {
  int res;

  tsock = socket(AF_INET, SOCK_DGRAM, 0);
  if (tsock < 0) {
    fprintf(stderr, "multi sock");
  }

  multicast_addr.sin_family = AF_INET;
  multicast_addr.sin_port = htons(DATA_PORT);
  res = inet_aton(MCAST_ADDR, &multicast_addr.sin_addr);
  if (res < 0) {
    fprintf(stderr, "inet aton");
  }
}

bool tread_from_stdin() {
  nuint64_t tmp{};
  int64_t pos;
  ssize_t rec_bytes;
  int c;

  tsock_mut.lock();

  pos = package_pos(read_bytes.nuint64 + PSIZE);
  package_t& package = packages[pos];

  memset(package.data, 0, PSIZE + AUDIO_DATA + 1);
  package.fbyte = 0; /* anyway we override the act data, which in
                        case of failure we don't want to retransmit */
  rec_bytes = 0;
  do {
    c = getchar();
    if (c == EOF) {
      printf("End of data\n");
      tsock_mut.unlock();
      return false;
    }

    package.data[AUDIO_DATA + rec_bytes] = static_cast<char>(c);
    rec_bytes++;
    rec_bytes += read(STDIN, &package.data[AUDIO_DATA + rec_bytes], PSIZE - rec_bytes);
  } while (rec_bytes != PSIZE);

  memcpy(package.data, SESSION_ID.nuint8, sizeof(SESSION_ID));
  tmp.nuint32[0] = htonl(read_bytes.nuint32[0]);
  tmp.nuint32[1] = htonl(read_bytes.nuint32[1]);
  memcpy(&package.data[sizeof(tmp)], tmp.nuint8, sizeof(tmp));

  package.fbyte = read_bytes.nuint64;
  read_bytes.nuint64 += PSIZE;

  send_package(package);

  tsock_mut.unlock();

  return true;
}

void transmitter() {
  tinit();

  std::thread rthread{retransmitter};

  while (data_left) {
    data_left = tread_from_stdin();
  }

  rthread.join();
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  MAIN                                                              *
 *--------------------------------------------------------------------------------------------------------------------*/
int main() {
  init();

  std::thread tthread{transmitter};
  controler();

  tthread.join();
  return 0;
}