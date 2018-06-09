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
uint16_t UI_PORT;
uint64_t BSIZE;
uint64_t RTIME;
uint64_t max_packages_no;
std::string DISCOVER_ADDR;


/* shared (receiver && discover) */
std::vector<package_t> packages;
uint64_t byte_zero;
uint64_t pexp_byte; /* determinates player head position */
uint64_t bbyte;

/* shared (receiver && interface) */
std::atomic<bool> switch_sender;

/* shared (interface && discover) */
transmitter_t* transmitters;
std::atomic<int> picked;
std::atomic<int> transmitters_no;
std::atomic<bool> refresh;
std::mutex trans_mut;

/* shared (receiver && retransmitter */
int retsock;
sockaddr_in ret_addr;
std::mutex ret_mut;

/* discover */
int dsock;
sockaddr_in dremote_addr;

/* receiver */
int rsock;
pollfd rpoll[RPOLL_SIZE];
uint64_t session_id;
sockaddr_in rlocal_addr;
uint64_t psize;

/* interface */
int isock;
pollfd ipoll[_POSIX_OPEN_MAX];
sockaddr_in ilocal_addr;

int64_t package_pos(uint64_t fbyte) {
  int64_t pos;
  assert(psize != 0);
  pos = ((fbyte - byte_zero) / psize) % max_packages_no;
  assert(pos >= 0 && pos < max_packages_no);
  return pos;
}

void debug_print_packages(bool play) {
  int64_t phead_pos;
  char white_space;

  phead_pos = package_pos(pexp_byte);
  for (int i = 0; i < max_packages_no; ++i) {
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

void debug_print_transmitters() {
  transmitter_t *ptr;

  fprintf(stderr, "\nTransmitters:\n"); fflush(stdout);
  ptr = transmitters;
  while (ptr) {
    fprintf(stderr, "%s", ptr->station_name.c_str());
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

void init(int argc, char** argv) {
  namespace po = boost::program_options;
  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help", "produce help message")
      (",d", po::value<std::string>(&DISCOVER_ADDR)->default_value("255.255.255.255"))
      (",C", po::value<uint16_t>(&CTRL_PORT)->default_value(37075), "set CTRL_PORT")
      (",b", po::value<uint64_t>(&BSIZE)->default_value(500000), "set BSIZE")
      (",R", po::value<uint64_t>(&RTIME)->default_value(250), "set RTIME")
      (",U", po::value<uint16_t>(&UI_PORT)->default_value(17075), "set UI_PORT");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (std::exception& e) {
    fprintf(stderr, "Invalid program options");
  }

  transmitters = nullptr;
  picked = -1;
  psize = 0;
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  RETRANSMITTER                                                    *
 *--------------------------------------------------------------------------------------------------------------------*/
void retinit() {
  int res;

  ret_mut.lock();

  retsock = socket(AF_INET, SOCK_DGRAM, 0);
  if (retsock < 0) {
    fprintf(stderr, "dsock");
  }

  ret_addr.sin_family = AF_INET;
  rlocal_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  ret_addr.sin_port = htons(0);

  res = bind(retsock, reinterpret_cast<const sockaddr *>(&ret_addr), sizeof(ret_addr));
  if (res < 0) {
    fprintf(stderr, "bind retransmitter");
  }

  ret_mut.unlock();
}

void ret_send_retransmition_requests() {
  socklen_t trans_addr_len;
  int flags;
  ssize_t snd_len;
  uint64_t retransmit_packages_no;
  int64_t act_pos;
  uint64_t act_exp_bytes;

  ret_mut.lock();

  act_exp_bytes = pexp_byte;
  act_pos = package_pos(act_exp_bytes);
  retransmit_packages_no = 0;
  std::string request = "LOURDER_PLEASE ";

  while (act_exp_bytes < bbyte) {
    if (packages[act_pos].fbyte != act_exp_bytes) {
      retransmit_packages_no++;
      request += std::to_string(act_exp_bytes) + ",";
    }
    act_pos = next(1, act_pos, max_packages_no);
    act_exp_bytes += psize;
  }

  if (retransmit_packages_no == 0) {
    ret_mut.unlock();
    return;
  }

  request[request.size() - 1] = EOL; /* delete last coma */
  trans_addr_len = sizeof(dremote_addr);
  flags = 0;
  snd_len = sendto(retsock, request.c_str(), request.size(), flags, (const struct sockaddr *) &ret_addr,
                   trans_addr_len);
  if (snd_len != request.size()) {
    fprintf(stderr, "partial/failed write");
  }

  ret_mut.unlock();
}

void retransmit() {
  retinit();
  while (true) {
    if (picked != -1 && psize != 0) {
      ret_send_retransmition_requests();
    }
    usleep(static_cast<__useconds_t>(RTIME));
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

  res = getaddrinfo(DISCOVER_ADDR.c_str(), nullptr, &dinfo_hints, &dinfo_res);
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

  if (fptr && fptr->mcast_addr.compare(transmitter->mcast_addr) == 0) {
    fptr->last_heard = 0;
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

  if (pos <= picked || picked == -1) {
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
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || rcv_len == 0) {
      read_replies = false;
    }  else {
      fprintf(stderr, "recvfrom discover socket");
    }
  } while (read_replies);

  trans_mut.unlock();
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
        if (pos == picked) {
          switch_sender = true;
        }
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
    if (picked == 0) {
      switch_sender = true;
    }
    transmitters_no--;
    if (transmitters_no == 0) {
      picked--;
    }
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

    dremove_unused_transmitters();
  }
}


/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  RECEIVER                                                          *
 *--------------------------------------------------------------------------------------------------------------------*/
void rinit() {
  rlocal_addr.sin_family = AF_INET;
  rlocal_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  rpoll[0].fd = rsock;
  rpoll[0].events = POLLIN;
  rpoll[STDOUT].fd = STDOUT;
  rpoll[STDOUT].events = 0;
}

void rmove_phead() {
  if (bbyte >= pexp_byte + max_packages_no * psize) {
    pexp_byte = bbyte - max_packages_no * psize + psize;
  }
}

bool rhole_in_data() {
  int64_t player_head;

  player_head = package_pos(pexp_byte);

  if (packages[player_head].sid != session_id) {
    return true;
  }

  return packages[player_head].fbyte != pexp_byte;
}

void rresize_buffor() {
  int i;

  for (i = 0; i < packages.size(); ++i) {
    delete[](packages[i].data);
  }
  packages.clear();

  max_packages_no = BSIZE / psize;
  packages.resize(max_packages_no);
  
  for (i = 0; i < packages.size(); ++i) {
    packages[i].data = new char[psize + 1];
  }
}

void receive() {
  int flags, ret;
  ssize_t rcv_len;
  int64_t pos;
  char message[BUF_SIZE + 1];
  bool play;
  uint64_t tmp_sid;
  uint64_t tmp_fbyte;
  sockaddr_in transmitter_addr;
  socklen_t transmitter_addr_len;

  flags = 0;
  play = false;
  transmitter_addr_len = sizeof(transmitter_addr);

  while (!switch_sender) {
    rpoll[0].revents = 0;
    rpoll[STDOUT].revents = 0;
    ret = poll(rpoll, RPOLL_SIZE, NO_LIMIT);
    if (ret < 0) {
      fprintf(stderr, "receiver ret");
    }

    if (rpoll[0].revents & POLLIN) {
      memset(message, 0, sizeof(message));
      rcv_len = recvfrom(rsock, message, BUF_SIZE, flags,
                         reinterpret_cast<sockaddr *>(&transmitter_addr), &transmitter_addr_len);
      if (rcv_len > 0) {
        memcpy(&tmp_sid, message, sizeof(uint64_t));
        tmp_sid = be64toh(tmp_sid);

        memcpy(&tmp_fbyte, &message[sizeof(uint64_t)], sizeof(uint64_t));
        tmp_fbyte = be64toh(tmp_fbyte);

        if (tmp_sid < session_id || tmp_fbyte < pexp_byte) {
          continue;
        } else if (tmp_sid > session_id) {
          trans_mut.lock();
          session_id = tmp_sid;
          pexp_byte = bbyte = byte_zero = tmp_fbyte;
          play = false;
          rpoll[STDOUT].events = 0;
          psize = static_cast<uint64_t>(rcv_len - AUDIO_DATA);
          ret_addr = transmitter_addr;
          rresize_buffor();
          trans_mut.unlock();
        }

        pos = package_pos(tmp_fbyte);
        package_t &package = packages[pos];

        package.sid = tmp_sid;
        package.fbyte = tmp_fbyte;
        memcpy(package.data, &message[AUDIO_DATA], psize);

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
        write(STDOUT, packages[package_pos(pexp_byte)].data, psize);
        pexp_byte += psize;
      }
    }
  }

  close(rsock);
}

void rjoin_group() {
  int res, act_pos;
  ip_mreq ip_mreq;
  transmitter_t* ptr;
  std::string mcast_addr;
  uint16_t mcast_port;

  session_id = 0;
  byte_zero = 0;
  pexp_byte = 0;

  act_pos = 0;

  trans_mut.lock();
  if (picked == -1) {
    trans_mut.unlock();
    return;
  }

  rsock = socket(AF_INET, SOCK_DGRAM, 0);
  if (rsock < 0) {
    fprintf(stderr, "receiver sock");
  }

  ptr = transmitters;
  while (act_pos != picked) {
    ptr = ptr->next;
    act_pos++;
  }

  mcast_addr = ptr->mcast_addr;
  mcast_port = ptr->data_port;

  rlocal_addr.sin_port = htons(mcast_port);

  ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  res = inet_aton(mcast_addr.c_str(), &ip_mreq.imr_multiaddr);
  if (res == 0) {
    fprintf(stderr, "receiver inet_aton");
  }

  res = setsockopt(rsock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &ip_mreq, sizeof(ip_mreq));
  if (res < 0) {
    fprintf(stderr, "receiver setsockopt add_membership");
  }

  res = bind(rsock, (const struct sockaddr *) &rlocal_addr, sizeof(rlocal_addr));
  if (res < 0) {
    fprintf(stderr, "bind receiver sock");
  }

  rpoll[0].fd = rsock;
  switch_sender = false;

  trans_mut.unlock();
}

void receiver() {
  rinit();

  while (true) {
    if (picked != -1) {
      rjoin_group();
      receive();
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
  char buf[BUF_SMALL_SIZE + 1];
  ssize_t res;

  std::string IAC_WILL_ECHO_SGA = "\377\373\001\377\373\003";
  std::string HIDE_CURSOR = "\e[?25l\n\r";

  cwrite(sock, IAC_WILL_ECHO_SGA.c_str(), IAC_WILL_ECHO_SGA.size());
  cwrite(sock, HIDE_CURSOR.c_str(), HIDE_CURSOR.size());

  res = read(sock, buf, BUF_SMALL_SIZE);
  if (res < 0) {
    fprintf(stderr, "reading from socket");
  }
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
      close(ipoll[pos].fd);
      ipoll[pos].fd = NOT_USED;
      ipoll[pos].revents = 0;
      break;

    case UP_ORD:
      if (picked > 0) {
        picked--;
        switch_sender = true;
        irefresh();
      }
      break;

    case DOWN_ORD:
      if (picked < transmitters_no - 1) {
        picked++;
        switch_sender = true;
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

  std::thread rthread{retransmit};
  std::thread ithread{interface};
  std::thread dthread{discover};
  receiver();

  ithread.join();
  dthread.join();
  return 0;
}