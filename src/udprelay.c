#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_NET_IF_H) && defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#define SET_INTERFACE
#endif

#include "utils.h"
#include "udprelay.h"
#include "cache.h"

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

static int verbose = 0;
static int client_conn = 0;
static int server_conn = 0;

int setnonblocking(int fd) {
    int flags;
    if (-1 ==(flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#ifdef SET_INTERFACE
int setinterface(int socket_fd, const char* interface_name)
{
    struct ifreq interface;
    memset(&interface, 0, sizeof(interface));
    strncpy(interface.ifr_name, interface_name, IFNAMSIZ);
    int res = setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(struct ifreq));
    return res;
}
#endif

int create_and_bind(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, server_sock;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC; /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_DGRAM; /* We want a UDP socket */

    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        LOGE("getaddrinfo: %s", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        server_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_sock == -1)
            continue;

        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
        setsockopt(server_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

        s = bind(server_sock, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            /* We managed to bind successfully! */
            break;
        } else {
            ERROR("bind");
        }

        close(server_sock);
    }

    if (rp == NULL) {
        LOGE("Could not bind");
        return -1;
    }

    freeaddrinfo(result);

    return server_sock;
}

struct client *connect_to_client(struct addrinfo *res, const char *iface) {
    int sockfd;
    int opt = 1;

    // initilize client socks
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        ERROR("socket");
        close(sockfd);
        return NULL;
    }

    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    struct client *client = new_client(sockfd);

    // setup client socks
    setnonblocking(sockfd);
#ifdef SET_INTERFACE
    if (iface) setinterface(sockfd, iface);
#endif

    connect(sockfd, res->ai_addr, res->ai_addrlen);

    return client;
}

static void server_recv_cb (EV_P_ ev_io *w, int revents) {
    struct server_ctx *server_recv_ctx = (struct server_ctx *)w;
    struct server *server = server_recv_ctx->server;
    struct client *client = NULL;

    int len = server->buf_len;
    char **buf = &server->buf;

    ev_timer_again(EV_A_ &server->recv_ctx->watcher);

    if (server->stage != 0) {
        client = server->client;
        buf = &client->buf;
        len = 0;
    }

    ssize_t r = recv(server->fd, *buf + len, BUF_SIZE - len, 0);

    if (r == 0) {
        // connection closed
        if (verbose) {
            LOGD("server_recv close the connection");
        }
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            ERROR("server recv");
            close_and_free_client(EV_A_ client);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    // handle incomplete header
    if (server->stage == 0) {
        r += server->buf_len;
        if (r <= enc_get_iv_len()) {
            // wait for more
            if (verbose) {
                LOGD("imcomplete header: %zu", r);
            }
            server->buf_len = r;
            return;
        } else {
            server->buf_len = 0;
        }
    }

    *buf = ss_decrypt(*buf, &r, server->d_ctx);

    if (*buf == NULL) {
        LOGE("invalid password or cipher");
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    }

    // handshake and transmit data
    if (server->stage == 5) {
        int s = send(client->fd, client->buf, r, 0);
        if (s == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no data, wait for send
                client->buf_len = r;
                client->buf_idx = 0;
                ev_io_stop(EV_A_ &server_recv_ctx->io);
                ev_io_start(EV_A_ &client->send_ctx->io);
            } else {
                ERROR("server_recv_send");
                close_and_free_client(EV_A_ client);
                close_and_free_server(EV_A_ server);
            }
        } else if (s < r) {
            client->buf_len = r - s;
            client->buf_idx = s;
            ev_io_stop(EV_A_ &server_recv_ctx->io);
            ev_io_start(EV_A_ &client->send_ctx->io);
        }
        return;

    } else if (server->stage == 0) {

        /*
         * Shadowsocks Protocol:
         *
         *    +------+----------+----------+
         *    | ATYP | DST.ADDR | DST.PORT |
         *    +------+----------+----------+
         *    |  1   | Variable |    2     |
         *    +------+----------+----------+
         */

        int offset = 0;
        char atyp = server->buf[offset++];
        char host[256];
        char port[64];
        memset(host, 0, 256);
        int p = 0;

        // get client addr and port
        if (atyp == 1) {
            // IP V4
            size_t in_addr_len = sizeof(struct in_addr);
            if (r > in_addr_len) {
                inet_ntop(AF_INET, (const void *)(server->buf + offset),
                        host, INET_ADDRSTRLEN);
                offset += in_addr_len;
            }
        } else if (atyp == 3) {
            // Domain name
            uint8_t name_len = *(uint8_t *)(server->buf + offset);
            if (name_len < r && name_len < 255 && name_len > 0) {
                memcpy(host, server->buf + offset + 1, name_len);
                offset += name_len + 1;
            }
        } else if (atyp == 4) {
            // IP V6
            size_t in6_addr_len = sizeof(struct in6_addr);
            if (r > in6_addr_len) {
                inet_ntop(AF_INET6, (const void*)(server->buf + offset), 
                        host, INET6_ADDRSTRLEN);
                offset += in6_addr_len;
            }
        }
        
        if (offset == 1) {
            LOGE("invalid header with addr type %d", atyp);
            close_and_free_server(EV_A_ server);
            return;
        }

        p = ntohs(*(uint16_t *)(server->buf + offset));
        offset += 2;

        sprintf(port, "%d", p);

        if (verbose) {
            LOGD("connect to: %s:%s", host, port);
        }

        struct addrinfo hints;
        asyncns_query_t *query;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        query = asyncns_getaddrinfo(server->server_ctx->asyncns,
                host, port, &hints);

        if (query == NULL) {
            ERROR("asyncns_getaddrinfo");
            close_and_free_server(EV_A_ server);
            return;
        }

        // XXX: should handle buffer carefully
        if (r > offset) {
            server->buf_len = r - offset;
            server->buf_idx = offset;
        }

        server->stage = 4;
        server->query = query;

        ev_io_stop(EV_A_ &server_recv_ctx->io);
        ev_timer_start(EV_A_ &server->send_ctx->watcher);

        return;
    }
    // should not reach here
    FATAL("server context error.");
}

static void server_send_cb (EV_P_ ev_io *w, int revents) {
    struct server_ctx *server_send_ctx = (struct server_ctx *)w;
    struct server *server = server_send_ctx->server;
    struct client *client = server->client;

    if (client == NULL) {
        LOGE("invalid server.");
        close_and_free_server(EV_A_ server);
        return;
    }

    if (server->buf_len == 0) {
        // close and free
        if (verbose) {
            LOGD("server_send close the connection");
        }
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(server->fd, server->buf + server->buf_idx,
                server->buf_len, 0);
        if (s < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ERROR("server_send_send");
                close_and_free_client(EV_A_ client);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < server->buf_len) {
            // partly sent, move memory, wait for the next time to send
            server->buf_len -= s;
            server->buf_idx += s;
            return;
        } else {
            // all sent out, wait for reading
            server->buf_len = 0;
            server->buf_idx = 0;
            ev_io_stop(EV_A_ &server_send_ctx->io);
            if (client != NULL) {
                ev_io_start(EV_A_ &client->recv_ctx->io);
                return;
            } else {
                LOGE("invalid client.");
                close_and_free_client(EV_A_ client);
                close_and_free_server(EV_A_ server);
                return;
            }
        }
    }
}

static void server_timeout_cb(EV_P_ ev_timer *watcher, int revents) {
    struct server_ctx *server_ctx = (struct server_ctx *) (((void*)watcher)
            - sizeof(ev_io));
    struct server *server = server_ctx->server;
    struct client *client = server->client;

    LOGE("TCP connection timeout");

    ev_timer_stop(EV_A_ watcher);

    close_and_free_client(EV_A_ client);
    close_and_free_server(EV_A_ server);
}

static void server_resolve_cb(EV_P_ ev_timer *watcher, int revents) {
    int err;
    struct addrinfo *result, *rp;
    struct server *server = (struct server *) ((void*)watcher);
    asyncns_t *asyncns = server->asyncns;
    asyncns_query_t *query = server->query;

    if (asyncns == NULL || query == NULL) {
        LOGE("invalid dns query.");
        close_and_free_server(EV_A_ server);
        return;
    }

    if (asyncns_wait(asyncns, 0) == -1) {
        // asyncns error
        FATAL("asyncns exit unexpectedly.");
    }

    if (!asyncns_isdone(asyncns, query)) {
        // wait for reolver
        return;
    }

    if (verbose) {
        LOGD("asyncns resolved.");
    }

    ev_timer_stop(EV_A_ watcher);

    err = asyncns_getaddrinfo_done(asyncns, query, &result);

    if (err) {
        ERROR("getaddrinfo");
        close_and_free_server(EV_A_ server);
    } else {
        // Use IPV4 address if possible
        for (rp = result; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family == AF_INET) break;
        }

        if (rp == NULL) {
            rp = result;
        }

        struct client *client = connect_to_client(rp, server->server_ctx->iface);

        if (client == NULL) {
            LOGE("connect error.");
            close_and_free_server(EV_A_ server);
        } else {
            server->client = client;
            client->server = server;

            // XXX: should handel buffer carefully
            if (server->buf_len > 0) {
                memcpy(client->buf, server->buf + server->buf_idx, server->buf_len);
                client->buf_len = server->buf_len;
                client->buf_idx = 0;
                server->buf_len = 0;
                server->buf_idx = 0;
            }

            // listen to client connected event
            ev_io_start(EV_A_ &client->send_ctx->io);
        }
    }

    // release addrinfo
    asyncns_freeaddrinfo(result);
}

static void client_recv_cb (EV_P_ ev_io *w, int revents) {
    struct client_ctx *client_recv_ctx = (struct client_ctx *)w;
    struct client *client = client_recv_ctx->client;
    struct server *server = client->server;

    if (server == NULL) {
        LOGE("invalid server.");
        close_and_free_client(EV_A_ client);
        return;
    }

    ev_timer_again(EV_A_ &server->recv_ctx->watcher);

    ssize_t r = recv(client->fd, server->buf, BUF_SIZE, 0);

    if (r == 0) {
        // connection closed
        if (verbose) {
            LOGD("client_recv close the connection");
        }
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            ERROR("client recv");
            close_and_free_client(EV_A_ client);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    server->buf = ss_encrypt(server->buf, &r, server->e_ctx);

    if (server->buf == NULL) {
        LOGE("invalid password or cipher");
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    }

    int s = send(server->fd, server->buf, r, 0);

    if (s == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data, wait for send
            server->buf_len = r;
            server->buf_idx = 0;
            ev_io_stop(EV_A_ &client_recv_ctx->io);
            ev_io_start(EV_A_ &server->send_ctx->io);
        } else {
            ERROR("client_recv_send");
            close_and_free_client(EV_A_ client);
            close_and_free_server(EV_A_ server);
        }
        return;
    } else if (s < r) {
        server->buf_len = r - s;
        server->buf_idx = s;
        ev_io_stop(EV_A_ &client_recv_ctx->io);
        ev_io_start(EV_A_ &server->send_ctx->io);
        return;
    }
}

static void client_send_cb (EV_P_ ev_io *w, int revents) {
    struct client_ctx *client_send_ctx = (struct client_ctx *)w;
    struct client *client = client_send_ctx->client;
    struct server *server = client->server;

    if (server == NULL) {
        LOGE("invalid server.");
        close_and_free_client(EV_A_ client);
        return;
    }

    if (!client_send_ctx->connected) {

        struct sockaddr_storage addr;
        socklen_t len = sizeof addr;
        int r = getpeername(client->fd, (struct sockaddr*)&addr, &len);
        if (r == 0) {
            if (verbose) {
                LOGD("client connected.");
            }
            client_send_ctx->connected = 1;

            if (client->buf_len == 0) {
                server->stage = 5;
                ev_io_stop(EV_A_ &client_send_ctx->io);
                ev_io_start(EV_A_ &server->recv_ctx->io);
                ev_io_start(EV_A_ &client->recv_ctx->io);
                return;
            }

        } else {
            ERROR("getpeername");
            // not connected
            close_and_free_client(EV_A_ client);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    if (client->buf_len == 0) {
        // close and free
        if (verbose) {
            LOGD("client_send close the connection");
        }
        close_and_free_client(EV_A_ client);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(client->fd, client->buf + client->buf_idx,
                client->buf_len, 0);
        if (s == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ERROR("client_send_send");
                // close and free
                close_and_free_client(EV_A_ client);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < client->buf_len) {
            // partly sent, move memory, wait for the next time to send
            client->buf_len -= s;
            client->buf_idx += s;
            return;
        } else {
            // all sent out, wait for reading
            client->buf_len = 0;
            client->buf_idx = 0;
            ev_io_stop(EV_A_ &client_send_ctx->io);
            if (server != NULL) {
                ev_io_start(EV_A_ &server->recv_ctx->io);
                if (server->stage == 4) {
                    server->stage = 5;
                    ev_io_start(EV_A_ &client->recv_ctx->io);
                }
            } else {
                LOGE("invalid server.");
                close_and_free_client(EV_A_ client);
                close_and_free_server(EV_A_ server);
            }
            return;
        }
    }
}

struct client* new_client(int fd) {
    client_conn++;
    struct client *client;
    client = malloc(sizeof(struct client));
    client->buf = malloc(BUF_SIZE);
    client->recv_ctx = malloc(sizeof(struct client_ctx));
    client->send_ctx = malloc(sizeof(struct client_ctx));
    client->fd = fd;
    ev_io_init(&client->recv_ctx->io, client_recv_cb, fd, EV_READ);
    ev_io_init(&client->send_ctx->io, client_send_cb, fd, EV_WRITE);
    client->recv_ctx->client = client;
    client->recv_ctx->connected = 0;
    client->send_ctx->client = client;
    client->send_ctx->connected = 0;
    client->buf_len = 0;
    client->buf_idx = 0;
    client->server = NULL;
    return client;
}

void free_client(struct client *client) {
    client_conn--;
    if (client != NULL) {
        if (client->server != NULL) {
            client->server->client = NULL;
        }
        if (client->buf != NULL) {
            free(client->buf);
        }
        free(client->recv_ctx);
        free(client->send_ctx);
        free(client);
    }
}

void close_and_free_client(EV_P_ struct client *client) {
    if (client != NULL) {
        ev_io_stop(EV_A_ &client->send_ctx->io);
        ev_io_stop(EV_A_ &client->recv_ctx->io);
        close(client->fd);
        free_client(client);
    }
    if (verbose) {
        LOGD("current client connection: %d", client_conn);
    }
}

struct server* new_server(int fd, struct server_ctx *ctx) {
    server_conn++;
    struct server *server;
    server = malloc(sizeof(struct server));
    server->buf = malloc(BUF_SIZE);
    ev_timer_init(&server->send_ctx->watcher, server_resolve_cb, 0.2, 0.5);
    ev_timer_init(&server->recv_ctx->watcher, server_timeout_cb, ctx->timeout, ctx->timeout * 5);
    server->query = NULL;
    server->server_ctx = ctx;
    if (ctx->method) {
        server->e_ctx = malloc(sizeof(struct enc_ctx));
        server->d_ctx = malloc(sizeof(struct enc_ctx));
        enc_ctx_init(listener->method, server->e_ctx, 1);
        enc_ctx_init(listener->method, server->d_ctx, 0);
    } else {
        server->e_ctx = NULL;
        server->d_ctx = NULL;
    }
    server->buf_len = 0;
    server->buf_idx = 0;
    server->client = NULL;
    return server;
}

void free_server(struct server *server) {
    server_conn--;
    if (server != NULL) {
        if (server->client != NULL) {
            server->client->server = NULL;
        }
        if (server->e_ctx != NULL) {
            EVP_CIPHER_CTX_cleanup(&server->e_ctx->evp);
            free(server->e_ctx);
        }
        if (server->d_ctx != NULL) {
            EVP_CIPHER_CTX_cleanup(&server->d_ctx->evp);
            free(server->d_ctx);
        }
        if (server->buf != NULL) {
            free(server->buf);
        }
        free(server->recv_ctx);
        free(server->send_ctx);
        free(server);
    }
}

void close_and_free_server(EV_P_ struct server *server) {
    if (server != NULL) {
        ev_io_stop(EV_A_ &server->send_ctx->io);
        ev_io_stop(EV_A_ &server->recv_ctx->io);
        ev_timer_stop(EV_A_ &server->send_ctx->watcher);
        ev_timer_stop(EV_A_ &server->recv_ctx->watcher);
        close(server->fd);
        free_server(server);
    }
    if (verbose) {
        LOGD("current server connection: %d", server_conn);
    }
}

static void server_cb (EV_P_ ev_io *w, int revents) {
    struct server_ctx *listener = (struct server_ctx *)w;
    int serverfd = accept(listener->fd, NULL, NULL);
    if (serverfd == -1) {
        ERROR("accept");
        return;
    }
    setnonblocking(serverfd);

    int opt = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(serverfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    if (verbose) {
        LOGD("accept a connection.");
    }

    struct server *server = new_server(serverfd, listener);
    ev_io_start(EV_A_ &server->recv_ctx->io);
    ev_timer_start(EV_A_ &server->recv_ctx->watcher);
}

int udprelay(const char *server_host, int server_num, const char *server_port, 
        int method, int timeout, const char *iface) {

    // inilitialize ev loop
    struct ev_loop *loop = EV_DEFAULT;

    // bind to each interface
    while (server_num > 0) {
        int index = --server_num;
        const char* host = server_host[index];

        // Bind to port
        int serverfd = create_and_bind(host, server_port);
        if (serverfd < 0) {
            FATAL("udprelay bind() error..");
        }
        setnonblocking(serverfd);

        // Setup proxy context
        struct server_ctx *server_ctx = malloc(sizeof(struct server_ctx));
        server_ctx->fd = serverfd;
        server_ctx->timeout = timeout;
        server_ctx->method = method;
        server_ctx->iface = iface;
        server_ctx->asyncns = asyncns;

        ev_io_init (&server_ctx.io, server_recv_cb, serverfd, EV_READ);
        ev_io_start (loop, &server_ctx.io);
    }

    // start ev loop
    ev_run (loop, 0);
    return 0;
}

