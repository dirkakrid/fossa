// Copyright (c) 2014 Cesanta Software Limited
// All rights reserved
//
// This library is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation. For the terms of this
// license, see <http://www.gnu.org/licenses/>.
//
// You are free to use this library under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this library under a commercial
// license, as set out in <http://cesanta.com/>.

#define _CRT_SECURE_NO_WARNINGS // Disable deprecation warning in VS2005+
#undef WIN32_LEAN_AND_MEAN      // Let windows.h always include winsock2.h

#ifdef _MSC_VER
#pragma warning (disable : 4127)  // FD_SET() emits warning, disable it
#pragma warning (disable : 4204)  // missing c99 support
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#ifndef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif
#ifndef __func__
#define STRX(x) #x
#define STR(x) STRX(x)
#define __func__ __FILE__ ":" STR(__LINE__)
#endif
#define snprintf _snprintf
typedef int socklen_t;
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned __int64 uint64_t;
typedef __int64   int64_t;
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>  // For inet_pton() when TS_ENABLE_IPV6 is defined
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#define INVALID_SOCKET (-1)
#define closesocket(x) close(x)
#define __cdecl
#endif

#ifdef TS_ENABLE_SSL
#ifdef __APPLE__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <openssl/ssl.h>
#endif

#include "tcp_skeleton.h"

#ifndef TS_MALLOC
#define TS_MALLOC malloc
#endif

#ifndef TS_REALLOC
#define TS_REALLOC realloc
#endif

#ifndef TS_FREE
#define TS_FREE free
#endif

#ifdef TS_ENABLE_DEBUG
#define DBG(x) do {           \
  printf("%-20s ", __func__); \
  printf x;                   \
  putchar('\n');              \
  fflush(stdout);             \
} while(0)
#else
#define DBG(x)
#endif

#ifndef IOBUF_RESIZE_MULTIPLIER
#define IOBUF_RESIZE_MULTIPLIER 2.0
#endif

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

#define ADD_CONNECTION(server, conn) do {                 \
  (conn)->next = (server)->active_connections;            \
  (server)->active_connections = (conn);                  \
  if ((conn)->next != NULL) (conn)->next->prev = (conn);  \
} while (0)

#define REMOVE_CONNECTION(conn) do {                                      \
  if ((conn)->prev) (conn)->prev->next = (conn)->next;                    \
  if ((conn)->next) (conn)->next->prev = (conn)->prev;                    \
  if (!(conn)->prev) (conn)->server->active_connections = (conn)->next;   \
} while (0)

union socket_address {
  struct sockaddr sa;
  struct sockaddr_in sin;
#ifdef TS_ENABLE_IPV6
  struct sockaddr_in6 sin6;
#endif
};

void iobuf_init(struct iobuf *iobuf, int size) {
  iobuf->len = iobuf->size = 0;
  iobuf->buf = NULL;

  if (size > 0 && (iobuf->buf = (char *) TS_MALLOC(size)) != NULL) {
    iobuf->size = size;
  }
}

void iobuf_free(struct iobuf *iobuf) {
  if (iobuf != NULL) {
    if (iobuf->buf != NULL) TS_FREE(iobuf->buf);
    iobuf_init(iobuf, 0);
  }
}

int iobuf_append(struct iobuf *io, const void *buf, int len) {
  static const double mult = IOBUF_RESIZE_MULTIPLIER;
  char *p = NULL;
  int new_len = 0;

  assert(io->len >= 0);
  assert(io->len <= io->size);

  //DBG(("1. %d %d %d", len, io->len, io->size));
  if (len <= 0) {
  } else if ((new_len = io->len + len) < io->size) {
    memcpy(io->buf + io->len, buf, len);
    io->len = new_len;
  } else if ((p = (char *)
              TS_REALLOC(io->buf, (int) (new_len * mult))) != NULL) {
    io->buf = p;
    memcpy(io->buf + io->len, buf, len);
    io->len = new_len;
    io->size = (int) (new_len * mult);
  } else {
    len = 0;
  }
  //DBG(("%d %d %d", len, io->len, io->size));

  return len;
}

void iobuf_remove(struct iobuf *io, int n) {
  if (n >= 0 && n <= io->len) {
    memmove(io->buf, io->buf + n, io->len - n);
    io->len -= n;
  }
}

static int call_user(struct ts_connection *conn, enum ts_event ev, void *p) {
  return conn->server->callback ? conn->server->callback(conn, ev, p) : -1;
}

static void close_conn(struct ts_connection *conn) {
  DBG(("%p %d", conn, conn->flags));
  call_user(conn, TS_CLOSE, NULL);
  REMOVE_CONNECTION(conn);
  closesocket(conn->sock);
  iobuf_free(&conn->recv_iobuf);
  iobuf_free(&conn->send_iobuf);
  TS_FREE(conn);
}

static void set_close_on_exec(int fd) {
#ifdef _WIN32
  (void) SetHandleInformation((HANDLE) fd, HANDLE_FLAG_INHERIT, 0);
#else
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
}

static void set_non_blocking_mode(int sock) {
#ifdef _WIN32
  unsigned long on = 1;
  ioctlsocket(sock, FIONBIO, &on);
#else
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Valid listening port spec is: [ip_address:]port, e.g. "80", "127.0.0.1:3128"
static int parse_port_string(const char *str, union socket_address *sa) {
  unsigned int a, b, c, d, port;
  int len = 0;
#ifdef TS_ENABLE_IPV6
  char buf[100];
#endif

  // MacOS needs that. If we do not zero it, subsequent bind() will fail.
  // Also, all-zeroes in the socket address means binding to all addresses
  // for both IPv4 and IPv6 (INADDR_ANY and IN6ADDR_ANY_INIT).
  memset(sa, 0, sizeof(*sa));
  sa->sin.sin_family = AF_INET;

  if (sscanf(str, "%u.%u.%u.%u:%u%n", &a, &b, &c, &d, &port, &len) == 5) {
    // Bind to a specific IPv4 address, e.g. 192.168.1.5:8080
    sa->sin.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    sa->sin.sin_port = htons((uint16_t) port);
#ifdef TS_ENABLE_IPV6
  } else if (sscanf(str, "[%49[^]]]:%u%n", buf, &port, &len) == 2 &&
             inet_pton(AF_INET6, buf, &sa->sin6.sin6_addr)) {
    // IPv6 address, e.g. [3ffe:2a00:100:7031::1]:8080
    sa->sin6.sin6_family = AF_INET6;
    sa->sin6.sin6_port = htons((uint16_t) port);
#endif
  } else if (sscanf(str, "%u%n", &port, &len) == 1) {
    // If only port is specified, bind to IPv4, INADDR_ANY
    sa->sin.sin_port = htons((uint16_t) port);
  } else {
    port = 0;   // Parsing failure. Make port invalid.
  }

  return port <= 0xffff && str[len] == '\0';
}

// 'sa' must be an initialized address to bind to
static int open_listening_socket(union socket_address *sa) {
  socklen_t len = sizeof(*sa);
  int on = 1, sock = INVALID_SOCKET;

  if ((sock = socket(sa->sa.sa_family, SOCK_STREAM, 6)) != INVALID_SOCKET &&
      !setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *) &on, sizeof(on)) &&
      !bind(sock, &sa->sa, sa->sa.sa_family == AF_INET ?
            sizeof(sa->sin) : sizeof(sa->sa)) &&
      !listen(sock, SOMAXCONN)) {
    set_non_blocking_mode(sock);
    // In case port was set to 0, get the real port number
    (void) getsockname(sock, &sa->sa, &len);
  } else if (sock != INVALID_SOCKET) {
    closesocket(sock);
    sock = INVALID_SOCKET;
  }

  return sock;
}

int ts_bind_to(struct ts_server *server, const char *str) {
  union socket_address sa;
  parse_port_string(str, &sa);
  server->listening_sock = open_listening_socket(&sa);
  return (int) ntohs(sa.sin.sin_port);
}

static struct ts_connection *accept_conn(struct ts_server *server) {
  struct ts_connection *c = NULL;
  union socket_address sa;
  socklen_t len = sizeof(sa);
  int sock = INVALID_SOCKET;

  // NOTE(lsm): on Windows, sock is always > FD_SETSIZE
  if ((sock = accept(server->listening_sock, &sa.sa, &len)) == INVALID_SOCKET) {
    closesocket(sock);
  } else if ((c = (struct ts_connection *) TS_MALLOC(sizeof(*c))) == NULL) {
    closesocket(sock);
  } else {
    set_close_on_exec(sock);
    set_non_blocking_mode(sock);
    memset(c, 0, sizeof(*c));
    c->server = server;
    c->sock = sock;
    c->flags |= TSF_ACCEPTED;
#if 0
    sockaddr_to_string(c->mg_conn.remote_ip,
                       sizeof(conn->mg_conn.remote_ip), &sa);
    c->mg_conn.remote_port = ntohs(sa.sin.sin_port);
    c->mg_conn.server_param = server->server_data;
    c->mg_conn.local_ip = server->local_ip;
    conn->mg_conn.local_port = ntohs(server->lsa.sin.sin_port);
#endif
    ADD_CONNECTION(server, c);
    call_user(c, TS_ACCEPT, NULL);
    DBG(("%p", c));
  }

  return c;
}

static int is_error(int n) {
  return n == 0 ||
    (n < 0 && errno != EINTR && errno != EINPROGRESS &&
     errno != EAGAIN && errno != EWOULDBLOCK
#ifdef _WIN32
     && WSAGetLastError() != WSAEINTR && WSAGetLastError() != WSAEWOULDBLOCK
#endif
    );
}

#ifdef TS_ENABLE_HEXDUMP
static void hexdump(const struct ts_connection *conn, const void *buf,
                    int len, const char *marker) {
  const unsigned char *p = (const unsigned char *) buf;
  char path[500], date[100], ascii[17];
  FILE *fp;

#if 0
  if (!match_prefix(TS_ENABLE_HEXDUMP, strlen(TS_ENABLE_HEXDUMP),
                    conn->remote_ip)) {
    return;
  }

  snprintf(path, sizeof(path), "%s.%hu.txt",
           conn->mg_conn.remote_ip, conn->mg_conn.remote_port);
#endif
  snprintf(path, sizeof(path), "%p.txt", conn);

  if ((fp = fopen(path, "a")) != NULL) {
    time_t cur_time = time(NULL);
    int i, idx;

    strftime(date, sizeof(date), "%d/%b/%Y %H:%M:%S", localtime(&cur_time));
    fprintf(fp, "%s %s %d bytes\n", marker, date, len);

    for (i = 0; i < len; i++) {
      idx = i % 16;
      if (idx == 0) {
        if (i > 0) fprintf(fp, "  %s\n", ascii);
        fprintf(fp, "%04x ", i);
      }
      fprintf(fp, " %02x", p[i]);
      ascii[idx] = p[i] < 0x20 || p[i] > 0x7e ? '.' : p[i];
      ascii[idx + 1] = '\0';
    }

    while (i++ % 16) fprintf(fp, "%s", "   ");
    fprintf(fp, "  %s\n\n", ascii);

    fclose(fp);
  }
}
#endif

static void read_from_socket(struct ts_connection *conn) {
  char buf[2048];
  int n = 0;

  if (conn->flags & TSF_CONNECTING) {
    int ok = 1, ret;
    socklen_t len = sizeof(ok);

    conn->flags &= ~TSF_CONNECTING;
    ret = getsockopt(conn->sock, SOL_SOCKET, SO_ERROR, (char *) &ok, &len);
#ifdef TS_ENABLE_SSL
    if (ret == 0 && ok == 0 && conn->ssl != NULL) {
      int res = SSL_connect(conn->ssl), ssl_err = SSL_get_error(conn->ssl, res);
      //DBG(("%p res %d %d", conn, res, ssl_err));
      if (res == 1) {
        conn->flags = TSF_SSL_HANDSHAKE_DONE;
      } else if (res == 0 || ssl_err == 2 || ssl_err == 3) {
        conn->flags |= TSF_CONNECTING;
        return; // Call us again
      } else {
        ok = 1;
      }
    }
#endif
    call_user(conn, TS_CONNECT, &ok);
    if (ok != 0) {
      conn->flags |= TSF_CLOSE_IMMEDIATELY;
    }
    return;
  }
#if 0
#endif

#ifdef TS_ENABLE_SSL
  if (conn->ssl != NULL) {
    if (conn->flags & TSF_SSL_HANDSHAKE_DONE) {
      n = SSL_read((SSL *) conn->ssl, buf, sizeof(buf));
    } else {
      if (SSL_accept((SSL *) conn->ssl) == 1) {
        conn->flags |= TSF_SSL_HANDSHAKE_DONE;
      }
      return;
    }
  } else
#endif
  {
    n = recv(conn->sock, buf, sizeof(buf), 0);
  }

#ifdef TS_ENABLE_HEXDUMP
  hexdump(conn, buf, n, "<-");
#endif

  if (is_error(n)) {
#if 0
    if (conn->endpoint_type == EP_CLIENT && conn->local_iobuf.len > 0) {
      call_http_client_handler(conn, MG_DOWNLOAD_SUCCESS);
    }
#endif
    conn->flags |= TSF_CLOSE_IMMEDIATELY;
  } else if (n > 0) {
    iobuf_append(&conn->recv_iobuf, buf, n);
    call_user(conn, TS_RECV, NULL);
  }

  DBG(("%p %d %d", conn, n, conn->flags));
  call_user(conn, TS_RECV, NULL);
}

static void write_to_socket(struct ts_connection *conn) {
  struct iobuf *io = &conn->send_iobuf;
  int n = 0;

#ifdef TS_ENABLE_SSL
  if (conn->ssl != NULL) {
    n = SSL_write(conn->ssl, io->buf, io->len);
  } else
#endif
  { n = send(conn->sock, io->buf, io->len, 0); }


#ifdef TS_ENABLE_HEXDUMP
  hexdump(conn, io->buf, n, "->");
#endif

  if (is_error(n)) {
    conn->flags |= TSF_CLOSE_IMMEDIATELY;
  } else if (n > 0) {
    iobuf_remove(io, n);
    //conn->num_bytes_sent += n;
  }

  if (io->len == 0 && conn->flags & TSF_FINISHED_SENDING_DATA) {
    conn->flags |= TSF_CLOSE_IMMEDIATELY;
  }

  DBG(("%p Written %d of %d(%d): [%.*s ...]",
       conn, n, io->len, io->size, io->len < 40 ? io->len : 40, io->buf));
  call_user(conn, TS_SEND, NULL);
}

int ts_send(struct ts_connection *conn, const void *buf, int len) {
  return iobuf_append(&conn->send_iobuf, buf, len);
}

static void add_to_set(int sock, fd_set *set, int *max_fd) {
  FD_SET(sock, set);
  if (sock > *max_fd) {
    *max_fd = sock;
  }
}

int ts_server_poll(struct ts_server *server, int milli) {
  struct ts_connection *conn, *tmp_conn;
  struct timeval tv;
  fd_set read_set, write_set;
  int num_active_connections = 0, max_fd = -1;
  time_t current_time = time(NULL);

  if (server->listening_sock == INVALID_SOCKET) return 0;

  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  add_to_set(server->listening_sock, &read_set, &max_fd);

  for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;
    call_user(conn, TS_POLL, NULL);
    add_to_set(conn->sock, &read_set, &max_fd);
    if (conn->flags & TSF_CONNECTING) {
      add_to_set(conn->sock, &write_set, &max_fd);
    }
#if 0
    if (conn->endpoint_type == EP_FILE) {
      transfer_file_data(conn);
    } else if (conn->endpoint_type == EP_CGI) {
      add_to_set(conn->endpoint.cgi_sock, &read_set, &max_fd);
    }
#endif
    if (conn->send_iobuf.len > 0 && !(conn->flags & TSF_BUFFER_BUT_DONT_SEND)) {
      add_to_set(conn->sock, &write_set, &max_fd);
    } else if (conn->flags & TSF_CLOSE_IMMEDIATELY) {
      close_conn(conn);
    }
  }

  tv.tv_sec = milli / 1000;
  tv.tv_usec = (milli % 1000) * 1000;

  if (select(max_fd + 1, &read_set, &write_set, NULL, &tv) > 0) {
    // Accept new connections
    if (FD_ISSET(server->listening_sock, &read_set)) {
      // We're not looping here, and accepting just one connection at
      // a time. The reason is that eCos does not respect non-blocking
      // flag on a listening socket and hangs in a loop.
      if ((conn = accept_conn(server)) != NULL) {
        conn->last_io_time = current_time;
      }
    }

    for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
      tmp_conn = conn->next;
      if (FD_ISSET(conn->sock, &read_set)) {
        conn->last_io_time = current_time;
        read_from_socket(conn);
      }
#if 0
#ifndef MONGOOSE_NO_CGI
      if (conn->endpoint_type == EP_CGI &&
          FD_ISSET(conn->endpoint.cgi_sock, &read_set)) {
        read_from_cgi(conn);
      }
#endif
#endif
      if (FD_ISSET(conn->sock, &write_set)) {
        if (conn->flags & TSF_CONNECTING) {
          read_from_socket(conn);
        } else if (!(conn->flags & TSF_BUFFER_BUT_DONT_SEND)) {
          conn->last_io_time = current_time;
          write_to_socket(conn);
        }
      }
    }
  }

  for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;

    if (conn->flags & TSF_CLOSE_IMMEDIATELY) {
      close_conn(conn);
    }

    num_active_connections++;
  }

  return num_active_connections;
}

int ts_connect(struct ts_server *server, const char *host, int port,
               int use_ssl, void *param) {
  int sock = INVALID_SOCKET;
  struct sockaddr_in sin;
  struct hostent *he = NULL;
  struct ts_connection *conn = NULL;
  int connect_ret_val;

  if (host == NULL || (he = gethostbyname(host)) == NULL ||
      (sock = socket(PF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
    DBG(("gethostbyname(%s) failed: %s", host, strerror(errno)));
    return 0;
  }
#ifndef MONGOOSE_USE_SSL
  if (use_ssl) return 0;
#endif

  sin.sin_family = AF_INET;
  sin.sin_port = htons((uint16_t) port);
  sin.sin_addr = * (struct in_addr *) he->h_addr_list[0];
  set_non_blocking_mode(sock);

  connect_ret_val = connect(sock, (struct sockaddr *) &sin, sizeof(sin));
  if (is_error(connect_ret_val)) {
    return 0;
  } else if ((conn = (struct ts_connection *)
              TS_MALLOC(sizeof(*conn))) == NULL) {
    closesocket(sock);
    return 0;
  }

  memset(conn, 0, sizeof(*conn));
  conn->server = server;
  conn->sock = sock;
  conn->connection_data = param;
  conn->flags = TSF_CONNECTING;

#ifdef TS_ENABLE_SSL
  if (use_ssl && (conn->ssl = SSL_new(server->client_ssl_ctx)) != NULL) {
    SSL_set_fd(conn->ssl, sock);
  }
#endif

  ADD_CONNECTION(server, conn);
  DBG(("%p %s:%d", conn, host, port));

  return 1;
}

void ts_server_init(struct ts_server *s, void *server_data, ts_callback_t cb) {
  memset(s, 0, sizeof(*s));
  s->listening_sock = INVALID_SOCKET;
  s->server_data = server_data;
  s->callback = cb;

#ifdef _WIN32
  { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
#else
  // Ignore SIGPIPE signal, so if client cancels the request, it
  // won't kill the whole process.
  signal(SIGPIPE, SIG_IGN);
#endif

#ifdef MONGOOSE_USE_SSL
  SSL_library_init();
  server->client_ssl_ctx = SSL_CTX_new(SSLv23_client_method());
#endif
}

void ts_server_free(struct ts_server *s) {
  struct ts_connection *conn, *tmp_conn;

  DBG(("%p", s));
  if (s == NULL) return;
  ts_server_poll(s, 0);

  if (s->listening_sock != INVALID_SOCKET) {
    closesocket(s->listening_sock);
  }

  for (conn = s->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;
    close_conn(conn);
  }

#ifndef TS_DISABLE_SOCKETPAIR
  //closesocket(s->ctl[0]);
  //closesocket(s->ctl[1]);
#endif

#ifdef TS_ENABLE_SSL
  if (s->ssl_ctx != NULL) SSL_CTX_free(s->ssl_ctx);
  if (s->client_ssl_ctx != NULL) SSL_CTX_free(s->client_ssl_ctx);
#endif
}