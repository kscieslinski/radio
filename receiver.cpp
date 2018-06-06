#include <netdb.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <zconf.h>
#include <cassert>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include "helper.h"


struct package_t {
  uint64_t sid;
  uint64_t fbyte;
  char* data;
};

struct transmitter_t {
  char mcast_addr[DOTTED_ADDR_SIZE + 1];
  int data_port;
  char station_name[STATION_MAX_NAME_SIZE + 1];
  int last_heard; /* in seconds */
  struct transmitter_t* next;
};

constexpr short MAX_NO_RESPONSE_TIME = 20;
constexpr short DISCOVER_LOOKUP_NAP = 5;

/* receiver run parameters */
uint16_t CTRL_PORT;
uint16_t DATA_PORT;
uint64_t PSIZE;
uint64_t BSIZE;
uint64_t RTIME;
uint64_t MAX_PACKAGES_NO;
char* MCAST_ADDR;
char* dADDR;

/* shared */
transmitter_t* transmitters;
std::vector<package_t> packages;
bool connected;
uint64_t byte_zero;
uint64_t pexp_byte; /* determinates player head position */
uint64_t bbyte;

/* discover */
int dsock;
sockaddr_in dremote_addr;

/* receiver */
int rsock;
uint64_t session_id;
sockaddr_in rlocal_addr;


int64_t package_pos(uint64_t fbyte) {
  int64_t pos;

  pos = ((fbyte - byte_zero) / PSIZE - 1) % MAX_PACKAGES_NO;
  assert(pos >= 0 && pos < MAX_PACKAGES_NO);
  return pos;
}

void init() {
  MCAST_ADDR = const_cast<char *>("239.10.11.12");
  dADDR = const_cast<char *>("255.255.255.255");
  RTIME = 250;
  CTRL_PORT = 37075;
  DATA_PORT = 27075;
  PSIZE = 512;// 10;
  BSIZE = 65536; //80;
  MAX_PACKAGES_NO = BSIZE / PSIZE;
  connected = false;
  transmitters = nullptr;

  packages.resize(MAX_PACKAGES_NO);
  for (int i = 0; i < MAX_PACKAGES_NO; ++i) {
    packages[i].data = new char[PSIZE + 1];
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  DISCOVER                                                          *
 *--------------------------------------------------------------------------------------------------------------------*/
void dinit() {
  int broadcast_val, flags, res;
  addrinfo dinfo_hints{};
  addrinfo* dinfo_res;

  memset(&dinfo_hints, 0, sizeof(dinfo_hints));
  dinfo_hints.ai_family = AF_INET;
  dinfo_hints.ai_socktype = SOCK_DGRAM;
  dinfo_hints.ai_protocol = IPPROTO_UDP;
  dinfo_hints.ai_flags = 0;
  dinfo_hints.ai_addrlen = 0;
  dinfo_hints.ai_addr = nullptr;
  dinfo_hints.ai_canonname = nullptr;
  dinfo_hints.ai_next = nullptr;

  res = getaddrinfo(dADDR, nullptr, &dinfo_hints, &dinfo_res);
  if (res < 0) {
    fprintf(stderr, "getaddrinfo");
  }
  dremote_addr.sin_family = AF_INET;
  dremote_addr.sin_addr.s_addr = ((struct sockaddr_in*) dinfo_res->ai_addr)->sin_addr.s_addr;
  dremote_addr.sin_port = htons((uint16_t) CTRL_PORT);

  freeaddrinfo(dinfo_res);

  dsock = socket(AF_INET, SOCK_DGRAM, 0);
  if (dsock < 0) {
    fprintf(stderr, "dsock");
  }

  broadcast_val = 1;
  res = setsockopt(dsock, SOL_SOCKET, SO_BROADCAST, &broadcast_val, sizeof(broadcast_val));
  if (res < 0) {
    fprintf(stderr, "setsockopt SO_BROADCAST");
  }

  flags = fcntl(dsock, F_GETFL, 0);
  flags |= O_NONBLOCK;
  fcntl(dsock, F_SETFL, flags);
}

void dsend_lookup() {
  socklen_t dremote_addr_len;
  int flags;
  ssize_t snd_len;
  char buffer[BUF_SIZE + 1];

  flags = 0;
  sprintf(buffer, "%s", "LOOKUP");
  dremote_addr_len = sizeof(dremote_addr);

  snd_len = sendto(dsock, buffer, strlen(buffer), flags, (const struct sockaddr *) &dremote_addr,
                   dremote_addr_len);
  if (snd_len != strlen(buffer)) {
    fprintf(stderr, "partial/failed write");
  }
}

void dmark_transmitter(char* buffer) {
  transmitter_t *ptr;
  transmitter_t *transmitter;
  const char* delimiter;

  transmitter = new transmitter_t();
  delimiter = " ";

  strcpy(transmitter->mcast_addr, strtok(buffer, delimiter));
  transmitter->data_port = atoi(strtok(nullptr, delimiter));
  strcpy(transmitter->station_name, strtok(nullptr, delimiter));

  ptr = transmitters;
  while (ptr) {
    if (strcmp(ptr->station_name, transmitter->station_name) == 0) {
      transmitter->last_heard = 0;
      free(transmitter);
      return;
    }
    ptr = ptr->next;
  }
}

void dreceive_replies() {
  bool read_replies;
  socklen_t transmitter_addr_len;
  sockaddr_in transmitter_addr{};
  ssize_t rcv_len;
  char buffer[BUF_SIZE + 1];
  int flags;

  flags = 0;
  transmitter_addr_len = sizeof(transmitter_addr);

  read_replies = true;
  do {
    rcv_len = recvfrom(dsock, buffer, BUF_SIZE, flags, (sockaddr *) &transmitter_addr,
                       &transmitter_addr_len);

    if (rcv_len > 0) {
      buffer[rcv_len] = NULL_TERMINATOR;
      printf("CTRL_SOCK: %.*s from %s\n", (int) rcv_len, buffer, inet_ntoa(transmitter_addr.sin_addr));
      dmark_transmitter(buffer);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || rcv_len == 0) {
      read_replies = false;
    }  else {
      fprintf(stderr, "recvfrom discover socket");
    }
  } while (read_replies);
}

void dsend_retransmition_requests() {
  socklen_t dremote_addr_len;
  int flags;
  ssize_t snd_len;
  uint64_t retransmit_packages_no;
  int64_t act_pos;
  uint64_t act_exp_bytes;

  act_pos = package_pos(pexp_byte);
  act_exp_bytes = pexp_byte;
  retransmit_packages_no = 0;
  std::string request = "LOURDER_PLEASE ";

  while (act_exp_bytes < bbyte) {
    if (packages[act_pos].fbyte != act_exp_bytes) {
      retransmit_packages_no++;
      request += std::to_string(act_exp_bytes) + ",";
    }
    act_pos = next(1, act_pos, MAX_PACKAGES_NO);
    act_exp_bytes += PSIZE;
  }

  if (retransmit_packages_no == 0) {
    return;
  }

  request[request.size() - 1] = NULL_TERMINATOR; /* delete last coma */
  dremote_addr_len = sizeof(dremote_addr);
  flags = 0;
  //printf("%s\n", request.c_str());
  snd_len = sendto(dsock, request.c_str(), request.size(), flags, (const struct sockaddr *) &dremote_addr,
                   dremote_addr_len);
  if (snd_len != request.size()) {
    fprintf(stderr, "partial/failed write");
  }
}

void dremove_unused_transmitters() {
  struct transmitter_t* front;
  struct transmitter_t* back;

  if (!transmitters) {
    return;
  }

  back = transmitters;
  front = transmitters->next;
  while (front) {
    front->last_heard += DISCOVER_LOOKUP_NAP;
    if (front->last_heard > MAX_NO_RESPONSE_TIME) {
      back->next = front->next;
      free(front);
      front = back->next;
    } else {
      back = front;
      front = front->next;
    }
  }

  transmitters->last_heard += DISCOVER_LOOKUP_NAP;
  if (transmitters->last_heard > MAX_NO_RESPONSE_TIME) {
    front = transmitters;
    transmitters = transmitters->next;
    free(front);
  }
}

void discover() {
  dinit();

  while (true) {
    //dsend_lookup();
    //dreceive_replies();
    if (connected) {
      dsend_retransmition_requests();
    }
    usleep(RTIME);
    //dremove_unused_transmitters();
  }
}
/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  RECEIVER                                                          *
 *--------------------------------------------------------------------------------------------------------------------*/
void rinit() {
  int res;
  ip_mreq ip_mreq{};

  session_id = 0;
  byte_zero = 0;

  rlocal_addr.sin_family = AF_INET;
  rlocal_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  rlocal_addr.sin_port = htons(DATA_PORT);

  rsock = socket(AF_INET, SOCK_DGRAM, 0);
  if (rsock < 0) {
    fprintf(stderr, "receiver sock");
  }

  ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  res = inet_aton(MCAST_ADDR, &ip_mreq.imr_multiaddr);
  if (res == 0) {
    fprintf(stderr, "receiver inet_aton");
  }
  res = (setsockopt(rsock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &ip_mreq, sizeof(ip_mreq)));
  if (res < 0) {
    fprintf(stderr, "receiver setsockopt add_membership");
  }

  res = bind(rsock, (const struct sockaddr *) &rlocal_addr, sizeof(rlocal_addr));
  if (res < 0) {
    fprintf(stderr, "bind receiver sock");
  }
}

void rset_capturing_details() {
  int flags;
  ssize_t rcv_len;
  char buffer[PSIZE + AUDIO_DATA + 1];
  nuint64_t tmp{};

  flags = 0;
  memset(buffer, 0, PSIZE + AUDIO_DATA + 1);

  rcv_len = recvfrom(rsock, buffer, PSIZE + AUDIO_DATA + 1, flags, nullptr, nullptr);
  if (rcv_len < 0) {
    fprintf(stderr, "recvfrom receiver socket");
  }

  memcpy(tmp.nuint8, buffer, sizeof(tmp));
  tmp.nuint32[0] = ntohl(tmp.nuint32[0]);
  tmp.nuint32[1] = ntohl(tmp.nuint32[1]);
  session_id = tmp.nuint64;

  memcpy(tmp.nuint8, &buffer[sizeof(uint64_t)], sizeof(tmp));
  tmp.nuint32[0] = ntohl(tmp.nuint32[0]);
  tmp.nuint32[1] = ntohl(tmp.nuint32[1]);
  byte_zero = tmp.nuint64;

  //printf("\nSETTING: %ld\n", byte_zero);

  bbyte = byte_zero;
  pexp_byte = byte_zero + PSIZE;
  connected = true;
}

void rmove_phead() {
  if (bbyte >= pexp_byte + MAX_PACKAGES_NO * PSIZE) {
    pexp_byte = bbyte - MAX_PACKAGES_NO * PSIZE + PSIZE;
  }
}

void debug_print_packages() {
  printf("packages: ");
  for (int i = 0; i < MAX_PACKAGES_NO; ++i) {
    if (packages[i].sid != session_id) {
      printf("[ ], ");
    } else {
      printf("[%ld], ", packages[i].fbyte);
    }
  }
  printf("\n");
}

bool rhole_in_data() {
  int64_t player_head;

  player_head = package_pos(pexp_byte);

  if (packages[player_head].sid != session_id) {
    return true;
  }

  return packages[player_head].fbyte != pexp_byte;

}

void rstart_capturing() {
  int flags;
  ssize_t rcv_len;
  int64_t pos;
  char message[PSIZE + AUDIO_DATA + 1];
  bool play;
  nuint64_t tmp_sid{};
  nuint64_t tmp_fbyte{};

  flags = 0;
  play = false;

  while (true) {
    rcv_len = recvfrom(rsock, message, PSIZE + AUDIO_DATA + 1, flags, nullptr, nullptr);
    if (rcv_len > 0) {
      memcpy(tmp_sid.nuint8, message, sizeof(uint64_t));
      tmp_sid.nuint32[0] = ntohl(tmp_sid.nuint32[0]);
      tmp_sid.nuint32[1] = ntohl(tmp_sid.nuint32[1]);

      memcpy(tmp_fbyte.nuint8, &message[sizeof(uint64_t)], sizeof(uint64_t));
      tmp_fbyte.nuint32[0] = ntohl(tmp_fbyte.nuint32[0]);
      tmp_fbyte.nuint32[1] = ntohl(tmp_fbyte.nuint32[1]);

      if (tmp_sid.nuint64 < session_id || tmp_fbyte.nuint64 < pexp_byte) {
        continue;
      } else if (tmp_sid.nuint64 > session_id) {
        return;
      }

      pos = package_pos(tmp_fbyte.nuint64);
      package_t& package = packages[pos];

      package.sid = tmp_sid.nuint64;
      package.fbyte = tmp_fbyte.nuint64;
      memcpy(package.data, &message[AUDIO_DATA], PSIZE);

      if (package.fbyte > bbyte) {
        bbyte = package.fbyte;
        rmove_phead();
      }

      if (!play && package.fbyte >= byte_zero + (BSIZE * 3) / 4) {
        play = true;
      }
    } else if (rcv_len < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
      fprintf(stderr, "recvfrom receiver socket");
    }

    if (play) {
      if (rhole_in_data()) {
        return;
      } else {
        write(STDOUT, packages[package_pos(pexp_byte)].data, PSIZE);
        pexp_byte += PSIZE;
      }
    }
  }
}

void receiver() {
  rinit();

  while (true) {
    rset_capturing_details();
    rstart_capturing();
    connected = false;
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  MAIN                                                              *
 *--------------------------------------------------------------------------------------------------------------------*/
int main() {
  init();

  std::thread dthread{discover};
  receiver();

  dthread.join();
  return 0;
}