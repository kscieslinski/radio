
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
#include <unordered_set>
#include "boost/program_options.hpp"
#include <regex>
#include <iostream>

#include "helper.h"


constexpr short LOOKUP_ORD = 1;
constexpr short REXMIT_ORD = 2;
constexpr short NO_ORD = 3;
constexpr short TRANSMITTER_NAP = 1;
const std::regex lookup_pattern("^ZERO_SEVEN_COME_IN\n$");
const std::regex rexmit_pattern("^REXMIT (\\d[0-9]*,)*+\\d[0-9]*\n$");

struct package_t {
  uint64_t fbyte;
  char* data;
};


/* sender run parameters */
uint16_t DATA_PORT;
uint16_t CTRL_PORT;
uint64_t PSIZE;
uint64_t FSIZE;
uint64_t RTIME;
std::string MCAST_ADDR;
std::string SNAME;

uint64_t MAX_PACKAGES_NO;
nuint64_t SESSION_ID;

/* shared */
std::vector<package_t> packages;
nuint64_t read_bytes;
bool data_left;

std::unordered_set<uint64_t> rexmit_fresh;
std::unordered_set<uint64_t> rexmit_old;
std::mutex rexmit_mut;

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

  snd_len = sendto(tsock, package.data, PSIZE + AUDIO_DATA + 1, flags,
                   (const struct sockaddr *) &multicast_addr, multicast_address_len);
  if (snd_len != PSIZE + AUDIO_DATA + 1) {
    fprintf(stderr, "sendto multicaster");
  }
}

void init(int argc, char** argv) {
  namespace po = boost::program_options;

  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help", "produce help message")
      (",a", po::value<std::string>(&MCAST_ADDR)->required()->default_value("239.10.11.12"), "set MCAST_ADDR")
      (",P", po::value<uint16_t>(&DATA_PORT)->default_value(27075), "set DATA_PORT")
      (",C", po::value<uint16_t>(&CTRL_PORT)->default_value(37075), "set CTRL_PORT")
      (",p", po::value<uint64_t>(&PSIZE)->default_value(10), "set PSIZE")
      (",f", po::value<uint64_t>(&FSIZE)->default_value(50), "set FSIZE")
      (",R", po::value<uint64_t>(&RTIME)->default_value(250), "set RTIME")
      (",n", po::value<std::string>(&SNAME)->default_value("Nienazwany Nadajnik"));

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help")) {
      std::cout << "Sender" << std::endl << desc << std::endl;
      exit(0);
    }

    po::notify(vm);
    if (PSIZE == 0 || !inet_aton(MCAST_ADDR.c_str(), &multicast_addr.sin_addr)) {
      std::cerr << "Invalid program options" << std::endl;
      exit(1);
    }
  } catch (std::exception& e) {
    fprintf(stderr, "Invalid program options\n");
    exit(1);
  }

  data_left = true;

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
  int val, res;
  sockaddr_in self_address{};

  self_address.sin_family = AF_INET;
  self_address.sin_addr.s_addr = htonl(INADDR_ANY);
  self_address.sin_port = htons(CTRL_PORT);

  csock = socket(AF_INET, SOCK_DGRAM, 0);
  if (csock < 0) {
    fprintf(stderr, "ctrl_sock");
  }

  val = 1;
  if (setsockopt(csock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
    fprintf(stderr, "reusing csock");
  }

  res = setsockopt(csock, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val));
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
  fprintf(stderr, "CTRL: %s", buff);
  if (std::regex_match(buff, lookup_pattern)) {
    return LOOKUP_ORD;
  } else if (std::regex_match(buff, rexmit_pattern)) {
    return REXMIT_ORD;
  } else {
    return NO_ORD;
  }
}

void cperform_lookup_ord(struct sockaddr_in& rec_addr) {
  int flags;
  ssize_t snd_len;
  socklen_t rec_addr_len;
  std::string reply;

  flags = 0;
  rec_addr_len = sizeof(rec_addr);
  reply = "BOREWICZ_HERE " + MCAST_ADDR + " " + std::to_string(DATA_PORT) + " " + SNAME + "\n";

  snd_len = sendto(csock, reply.c_str(), reply.size(), flags, (const struct sockaddr *) &rec_addr, rec_addr_len);
  if (snd_len != reply.size()) {
    fprintf(stderr, "sendto lookup");
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
  rexmit_mut.lock();
  while (token) {
    package_to_retransmit = (uint64_t) std::stoull(token);
    rexmit_fresh.insert(package_to_retransmit);
    token = strtok(nullptr, coma_delimiter);
  }
  rexmit_mut.unlock();
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

  while (true)/*(data_left)*/ {
    memset(buff, 0, BUF_SIZE + 1);
    order = cread_order(rec_addr, buff);
    cperform_order(static_cast<short>(order), buff, rec_addr);
  }
}

/* -------------------------------------------------------------------------------------------------------------------*
 *                                                  RETRANSMITTER                                                     *
 *--------------------------------------------------------------------------------------------------------------------*/
void retransmit() {
  int64_t pos;

  rexmit_mut.lock();
  rexmit_old = std::move(rexmit_fresh);
  rexmit_fresh.clear();
  rexmit_mut.unlock();

  tsock_mut.lock();

  for (uint64_t fbyte : rexmit_old) {
    pos = package_pos(fbyte);
    if (fbyte == packages[pos].fbyte) {
      send_package(packages[pos]);
    }
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
  res = inet_aton(MCAST_ADDR.c_str(), &multicast_addr.sin_addr);
  if (res < 0) {
    fprintf(stderr, "inet aton");
  }
}

bool tread_from_stdin() {
  nuint64_t tmp{};
  int64_t pos;
  int c;

  tsock_mut.lock();

  pos = package_pos(read_bytes.nuint64 + PSIZE);
  package_t& package = packages[pos];

  memset(package.data, 0, PSIZE + AUDIO_DATA + 1);
  package.fbyte = 0; /* anyway we override the act data, which in
                        case of failure we don't want to retransmit */

  for (int i = 0; i < PSIZE; ++i) {
    c = getchar();
    if (c == EOF) {
      printf("End of data\n");
      tsock_mut.unlock();
      return false;
    }

    package.data[AUDIO_DATA + i] = static_cast<char>(c);
  }

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
int main(int argc, char** argv) {
  init(argc, argv);

  //std::thread tthread{transmitter};
  controler();

  //tthread.join();
  return 0;
}