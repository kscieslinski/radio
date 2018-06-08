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
#include <poll.h>
#include <sys/time.h>
#include <boost/program_options.hpp>
#include <atomic>
#include <mutex>
#include "helper.h"


struct package_t {
  uint64_t sid;
  uint64_t fbyte;
  char* data;
};

struct transmitter_t {
  std::string station_name;
  std::string mcast_addr;
  uint16_t data_port;
  int last_heard; /* in seconds */
  struct transmitter_t* next;
};

constexpr int MAX_NO_RESPONSE_TIME = 20;
constexpr int DISCOVER_LOOKUP_NAP = 5;
constexpr int RPOLL_SIZE = 2;
constexpr int NO_LIMIT = -1;
constexpr int QUEUE_LNG = 5;
constexpr int CENTRAL = 0;
constexpr int CLOSE_CON_ORDER = 1;
constexpr int UP_ORD = 2;
constexpr int DOWN_ORD = 3;


/* receiver run parameters */
uint16_t CTRL_PORT;
uint16_t DATA_PORT;
uint16_t UI_PORT;
uint64_t PSIZE;
uint64_t BSIZE;
uint64_t RTIME;
uint64_t MAX_PACKAGES_NO;
char* MCAST_ADDR;
char* dADDR;


/* shared (receiver && discover) */
std::vector<package_t> packages;
uint64_t byte_zero;
uint64_t pexp_byte; /* determinates player head position */
uint64_t bbyte;

/* shared (interface && discover) */
transmitter_t* transmitters;
std::atomic<int> picked;
std::atomic<int> transmitters_no;
std::atomic<bool> refresh;
std::mutex trans_mut;


/* discover */
int dsock;
sockaddr_in dremote_addr;

/* receiver */
int rsock;
pollfd rpoll[RPOLL_SIZE];
uint64_t session_id;
sockaddr_in rlocal_addr;

/* interface */
int isock;
pollfd ipoll[_POSIX_OPEN_MAX];
sockaddr_in ilocal_addr;

void debug_print_transmitters() {
  transmitter_t *ptr;

  printf("\nTransmitters:\n"); fflush(stdout);
  ptr = transmitters;
  while (ptr) {
    printf("%s", ptr->station_name.c_str());
    ptr = ptr->next;
  }

}

void cwrite(int sock, const char* msg, size_t msg_size) {
  ssize_t len;

  len = write(sock, msg, msg_size);
  if (len != msg_size) {
    fprintf(stderr, "writing to socket interface\n");
  }
}

int64_t package_pos(uint64_t fbyte) {
  int64_t pos;

  pos = ((fbyte - byte_zero) / PSIZE) % MAX_PACKAGES_NO;
  assert(pos >= 0 && pos < MAX_PACKAGES_NO);
  return pos;
}

void init(int argc, char** argv) {
  namespace po = boost::program_options;
  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help", "produce help message")
      (",C", po::value<uint16_t>(&CTRL_PORT)->default_value(37075), "set CTRL_PORT")
      (",b", po::value<uint64_t>(&BSIZE)->default_value(80), "set BSIZE")
      (",R", po::value<uint64_t>(&RTIME)->default_value(250), "set RTIME")
      (",U", po::value<uint16_t>(&UI_PORT)->default_value(17075), "set UI_PORT");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (std::exception& e) {
    fprintf(stderr, "Invalid program options");
  }

  MCAST_ADDR = const_cast<char *>("239.10.11.12");
  dADDR = const_cast<char *>("255.255.255.255");
  DATA_PORT = 27075;
  PSIZE = 10;//512;// 10;
  MAX_PACKAGES_NO = BSIZE / PSIZE;
  transmitters = nullptr;
  picked = -1;

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
  std::string request;

  request = "ZERO_SEVEN_COME_IN\n";

  flags = 0;
  dremote_addr_len = sizeof(dremote_addr);

  snd_len = sendto(dsock, request.c_str(), request.size(), flags, (const struct sockaddr *) &dremote_addr,
                   dremote_addr_len);
  if (snd_len != request.size()) {
    fprintf(stderr, "partial/failed write");
  }
}

void dmark_transmitter(char* buffer) {
  transmitter_t *fptr;
  transmitter_t *bptr;
  transmitter_t *transmitter;
  int pos;
  const char* delimiter;

  transmitter = new transmitter_t();
  transmitter->next = nullptr;
  delimiter = " ";

  strtok(buffer, delimiter);
  transmitter->mcast_addr = std::string(strtok(nullptr, delimiter));
  transmitter->data_port = static_cast<uint16_t>(std::stoi(strtok(nullptr, delimiter)));
  transmitter->station_name = std::string(strtok(nullptr, delimiter));

  if (!transmitters) {
    transmitters = transmitter;
    transmitters_no++;
    picked++;
    refresh = true;
    return;
  }

  bptr = nullptr;
  fptr = transmitters;
  pos = 0;
  while (fptr && fptr->station_name.compare(transmitter->station_name) < 0) {
    bptr = fptr;
    fptr = fptr->next;
    pos++;
  }

  if (fptr && fptr->station_name.compare(transmitter->station_name) == 0) {
    transmitter->last_heard = 0;
    delete(transmitter);
    return;
  }

  if (!bptr) {
    transmitters = transmitter;
    transmitter->next = fptr;
  } else {
    bptr->next = transmitter;
    transmitter->next = fptr;
  }

  if (pos < picked || picked == -1) {
    picked++;
  }
  transmitters_no++;
  refresh = true;
}

void dreceive_replies() {
  bool read_replies;
  socklen_t transmitter_addr_len;
  sockaddr_in transmitter_addr{};
  ssize_t rcv_len;
  char buffer[BUF_SMALL_SIZE + 1];
  int flags;

  trans_mut.lock();
  flags = 0;
  transmitter_addr_len = sizeof(transmitter_addr);


  read_replies = true;
  do {
    rcv_len = recvfrom(dsock, buffer, BUF_SMALL_SIZE, flags, (struct sockaddr *) &transmitter_addr,
                       &transmitter_addr_len);

    if (rcv_len > 0) {
      buffer[rcv_len] = NULL_TERMINATOR;
      dmark_transmitter(buffer);
      debug_print_transmitters();
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || rcv_len == 0) {
      read_replies = false;
    }  else {
      fprintf(stderr, "recvfrom discover socket");
    }
  } while (read_replies);

  trans_mut.unlock();
}

void dsend_retransmition_requests() {
  socklen_t dremote_addr_len;
  int flags;
  ssize_t snd_len;
  uint64_t retransmit_packages_no;
  int64_t act_pos;
  uint64_t act_exp_bytes;

  act_exp_bytes = pexp_byte;
  act_pos = package_pos(act_exp_bytes);
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

  request[request.size() - 1] = EOL; /* delete last coma */
  dremote_addr_len = sizeof(dremote_addr);
  flags = 0;
  snd_len = sendto(dsock, request.c_str(), request.size(), flags, (const struct sockaddr *) &dremote_addr,
                   dremote_addr_len);
  if (snd_len != request.size()) {
    fprintf(stderr, "partial/failed write");
  }
}

void dremove_unused_transmitters() {
  struct transmitter_t* front;
  struct transmitter_t* back;
  int pos;

  if (!transmitters) {
    trans_mut.unlock();
    return;
  }

  pos = 0;
  back = transmitters;
  front = transmitters->next;
  while (front) {
    front->last_heard += DISCOVER_LOOKUP_NAP;
    if (front->last_heard > MAX_NO_RESPONSE_TIME) {
      back->next = front->next;
      if (pos <= picked) {
        picked--;
      }
      transmitters_no--;
      refresh = true;
      delete(front);
      front = back->next;
    } else {
      back = front;
      front = front->next;
      pos++;
    }
  }

  transmitters->last_heard += DISCOVER_LOOKUP_NAP;
  if (transmitters->last_heard > MAX_NO_RESPONSE_TIME) {
    front = transmitters;
    transmitters = transmitters->next;
    picked--;
    transmitters_no--;
    refresh = true;
    delete(front);
  }
}

void discover() {
  dinit();

  while (true) {
    dsend_lookup();
    dreceive_replies();
    sleep(DISCOVER_LOOKUP_NAP);
    //if (connected) {
    //  dsend_retransmition_requests();
    //}
    //usleep(static_cast<__useconds_t>(RTIME));
    dremove_unused_transmitters();
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
  pexp_byte = 0;

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

  rpoll[0].fd = rsock;
  rpoll[0].events = POLLIN;
  rpoll[STDOUT].fd = STDOUT;
  rpoll[STDOUT].events = 0;
}

void rmove_phead() {
  if (bbyte >= pexp_byte + MAX_PACKAGES_NO * PSIZE) {
    pexp_byte = bbyte - MAX_PACKAGES_NO * PSIZE + PSIZE;
  }
}

void debug_print_packages(bool play) {
  int64_t phead_pos;
  char white_space;

  phead_pos = package_pos(pexp_byte);
  fprintf(stderr, "play=%d\n", play);
  for (int i = 0; i < MAX_PACKAGES_NO; ++i) {
    if (i == phead_pos) {
      white_space = '>';
    } else {
      white_space = ' ';
    }

    if (packages[i].sid != session_id) {
      fprintf(stderr, "%c[ ]", white_space);
    } else {
      fprintf(stderr, "%c[%ld]", white_space, packages[i].fbyte);
    }
  }
  fprintf(stderr, "\n\n");
}

bool rhole_in_data() {
  int64_t player_head;

  player_head = package_pos(pexp_byte);

  if (packages[player_head].sid != session_id) {
    return true;
  }

  return packages[player_head].fbyte != pexp_byte;
}

void receiver() {
  int flags, ret;
  ssize_t rcv_len;
  int64_t pos;
  char message[PSIZE + AUDIO_DATA + 1];
  bool play;
  nuint64_t tmp_sid{};
  nuint64_t tmp_fbyte{};

  rinit();

  flags = 0;
  play = false;

  while (true) {
    debug_print_packages(play);
    rpoll[0].revents = 0;
    rpoll[STDOUT].revents = 0;
    ret = poll(rpoll, RPOLL_SIZE, NO_LIMIT);
    if (ret < 0) {
      fprintf(stderr, "receiver ret");
    }

    if (rpoll[0].revents & POLLIN) {
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
          session_id = tmp_sid.nuint64;
          pexp_byte = bbyte = byte_zero = tmp_fbyte.nuint64;
          play = false;
          rpoll[STDOUT].events = 0;
          fprintf(stderr, "\nSETTING: %ld\n", byte_zero);
        }

        pos = package_pos(tmp_fbyte.nuint64);
        package_t &package = packages[pos];

        package.sid = tmp_sid.nuint64;
        package.fbyte = tmp_fbyte.nuint64;
        memcpy(package.data, &message[AUDIO_DATA], PSIZE);

        if (package.fbyte > bbyte) {
          bbyte = package.fbyte;
          rmove_phead();
          if (!play && bbyte >= byte_zero + (BSIZE * 3) / 4) {
            play = true;
            rpoll[STDOUT].events = POLLOUT;
          }
        }
      } else if (rcv_len < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
        fprintf(stderr, "recvfrom receiver socket");
      }
    }

    if (rpoll[STDOUT].revents & POLLOUT && play) {
      if (rhole_in_data()) {
        session_id = 0;
        play = false;
        rpoll[STDOUT].events = 0;
      } else {
        write(STDOUT, packages[package_pos(pexp_byte)].data, PSIZE);
        pexp_byte += PSIZE;
      }
    }
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  INTERFACE                                                         *
 *--------------------------------------------------------------------------------------------------------------------*/
void iinit() {
  int res, i;

  refresh = false;

  isock = socket(PF_INET, SOCK_STREAM, 0);
  if (isock < 0) {
    fprintf(stderr, "sock interface");
  }

  ilocal_addr.sin_family = AF_INET;
  ilocal_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  ilocal_addr.sin_port = htons(UI_PORT);

  res = bind(isock, reinterpret_cast<const sockaddr *>(&ilocal_addr), sizeof ilocal_addr);
  if (res < 0) {
    fprintf(stderr, "bind interface");
  }

  res = listen(isock, QUEUE_LNG);
  if (res == -1) {
    fprintf(stderr, "listen interface");
  }

  for (i = 0; i < _POSIX_OPEN_MAX; ++i) {
    ipoll[i].fd = -1;
    ipoll[i].events = POLLIN;
    ipoll[i].revents = 0;
  }
  ipoll[CENTRAL].fd = isock;
}

void iconfigure_telnet(int sock) {
  std::string IAC_WILL_ECHO_SGA = "\377\373\001\377\373\003";
  std::string HIDE_CURSOR = "\e[?25l\n\r";

  cwrite(sock, IAC_WILL_ECHO_SGA.c_str(), IAC_WILL_ECHO_SGA.size());
  cwrite(sock, HIDE_CURSOR.c_str(), HIDE_CURSOR.size());
}

std::string iget_menu() {
  uint64_t act_pos;
  transmitter_t* ptr;
  std::string res, header, footer, info;

  trans_mut.lock();
  info = "picked=" + std::to_string(picked) + " transmitters_no=" + std::to_string(transmitters_no) + "\n\r";
  res.append(info);
  header = "------------------------------------------------------------------------\n\r"
           "  SIK Radio\n\r"
           "------------------------------------------------------------------------\n\r";

  footer = "------------------------------------------------------------------------\n\r";

  res.append(header);
  act_pos = 0;
  ptr = transmitters;

  while (ptr) {
    if (act_pos == picked) {
      res.append("   >" + ptr->station_name + "\r");
    } else {
      res.append("    " + ptr->station_name + "\r");
    }
    ptr = ptr->next;
    act_pos++;
  }
  res.append(footer);

  trans_mut.unlock();
  return res;
}

void irefresh() {
  std::string act_menu;

  act_menu = iget_menu();
  for (int i = 1; i < _POSIX_OPEN_MAX; ++i) {
    if (ipoll[i].fd != NOT_USED) {
      cwrite(ipoll[i].fd, act_menu.c_str(), act_menu.size());
    }
  }

  refresh = false;
}

void iadd_client() {
  std::string menu;

  int client_sock, i, res;

  client_sock = accept(isock, 0, 0);
  if (client_sock < 0) {
    fprintf(stderr, "accept interface");
  }

  for (i = 1; i < _POSIX_OPEN_MAX; ++i) {
    if (ipoll[i].fd == NOT_USED) {
      ipoll[i].fd = client_sock;
      iconfigure_telnet(client_sock);
      menu = iget_menu();
      cwrite(ipoll[i].fd, menu.c_str(), menu.size());
      return;
    }
  }

  fprintf(stderr, "to many clients interface");
  res = close(client_sock);
  if (res < 0) {
    fprintf(stderr, "close interface");
  }
}

int iread_order(int sock) {
  ssize_t rcv_bytes;
  char c;
  int step;

  step = 0;

  while (true) {
    rcv_bytes = read(sock, &c, 1);
    if (rcv_bytes < 0) {
      fprintf(stderr, "read order interface");
    }
    if (rcv_bytes == 0) {
      return CLOSE_CON_ORDER;
    }

    if ((step == 0 && c == ESC) || (step == 1 && c == '[')) {
      step++;
    } else if (step == 2 && c == 'A') {
      return UP_ORD;
    } else if (step == 2 && c == 'B') {
      return DOWN_ORD;
    } else {
      step = 0;
    }
  }
}

int iproc_order(int order, int pos) {
  switch (order) {
    case CLOSE_CON_ORDER:
      ipoll[pos].fd = NOT_USED;
      ipoll[pos].revents = 0;
      break;

    case UP_ORD:
      if (picked > 0) {
        picked--;
        irefresh();
      }
      break;

    case DOWN_ORD:
      if (picked < transmitters_no - 1) {
        picked++;
        irefresh();
      }
      break;

    default:
      fprintf(stderr, "order interface");
      break;
  }
}

void interface() {
  int ret, i, order;
  std::string menu;

  iinit();

  while(true) {
    for (i = 0; i < _POSIX_OPEN_MAX; ++i) {
      ipoll[i].revents = 0;
    }

    if (refresh) {
      irefresh();
    }

    ret = poll(ipoll, _POSIX_OPEN_MAX, 300);
    if (ret < 0) {
      fprintf(stderr, "poll interface");
    }

    if (ipoll[CENTRAL].revents & POLLIN) {
      iadd_client();
    }

    for (i = 1; i < _POSIX_OPEN_MAX; ++i) {
      if (ipoll[i].revents & POLLIN) {
        order = iread_order(ipoll[i].fd);
        iproc_order(order, i);
      }
    }
  }
}


/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  MAIN                                                              *
 *--------------------------------------------------------------------------------------------------------------------*/
int main(int argc, char** argv) {
  init(argc, argv);

  std::thread ithread{interface};
  std::thread dthread{discover};
  //receiver();

  ithread.join();
  dthread.join();
  return 0;
}