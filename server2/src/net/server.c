#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "frame pixels are sent in host order; the protocol is little-endian"
#endif

#define STATUS_PERIOD_S 1.0

typedef struct {
    int             fd;
    app_t          *app;
    uint8_t        *enc;            /* encoded pixels for integer dtypes */
    size_t          enc_cap;
    pthread_mutex_t send_lock;      /* one message at a time on the socket */
    atomic_int      alive;
} conn_t;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* Writes all iovecs completely. Returns 0, or -1 on error / closed peer. */
static int send_all(int fd, struct iovec *iov, int n)
{
    while (n > 0) {
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = (size_t)n };
        ssize_t k = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        while (n > 0 && (size_t)k >= iov->iov_len) {
            k -= iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov->iov_base = (char *)iov->iov_base + k;
            iov->iov_len -= k;
        }
    }
    return 0;
}

static int send_msg(conn_t *c, uint16_t type, const void *payload, size_t len)
{
    uint8_t hdr[PROTO_HDR_SIZE];
    proto_pack_hdr(hdr, type, (uint32_t)len);
    struct iovec iov[2] = { { hdr, sizeof(hdr) }, { (void *)payload, len } };
    pthread_mutex_lock(&c->send_lock);
    int rc = send_all(c->fd, iov, len ? 2 : 1);
    pthread_mutex_unlock(&c->send_lock);
    return rc;
}

static int send_frame(conn_t *c, const app_frame_t *f)
{
    size_t npix = (size_t)f->n_images * f->width * f->height;
    size_t psz = proto_pixel_size(f->dtype);
    float  lo, hi;
    if (!psz)
        return -1;
    proto_minmax(f->data, npix, &lo, &hi);

    void  *payload = f->data;                  /* float32: send the frame buffer directly */
    size_t data = npix * psz;
    if (f->dtype != 0) {
        if (c->enc_cap < data) {
            uint8_t *p = realloc(c->enc, data);
            if (!p)
                return -1;
            c->enc = p;
            c->enc_cap = data;
        }
        proto_encode(f->data, npix, f->dtype, lo, hi, c->enc);
        payload = c->enc;
    }
    uint8_t hdr[PROTO_HDR_SIZE], fhdr[PROTO_FHDR_SIZE];
    proto_pack_hdr(hdr, MSG_FRAME, (uint32_t)(PROTO_FHDR_SIZE + data));
    proto_pack_frame_hdr(fhdr, f, f->dtype, lo, hi);
    struct iovec iov[3] = { { hdr, sizeof(hdr) }, { fhdr, sizeof(fhdr) }, { payload, data } };
    pthread_mutex_lock(&c->send_lock);
    int rc = send_all(c->fd, iov, 3);
    pthread_mutex_unlock(&c->send_lock);
    return rc;
}

/* Frames as they become ready, plus a STATUS message every second. */
static void *sender_thread(void *arg)
{
    conn_t *c = arg;
    double  next_status = mono_s() + STATUS_PERIOD_S;
    char    st[1024];

    while (atomic_load(&c->alive)) {
        const app_frame_t *f = app_frame_wait(c->app, 100);
        if (f) {
            int ok = send_frame(c, f) == 0;
            app_frame_release(c->app, f, ok);
            if (!ok)
                break;
        }
        if (mono_s() >= next_status) {
            next_status += STATUS_PERIOD_S;
            app_status(c->app, st, sizeof(st));
            if (send_msg(c, MSG_STATUS, st, strlen(st)))
                break;
        }
    }
    atomic_store(&c->alive, 0);
    shutdown(c->fd, SHUT_RDWR);              /* wakes the reader */
    return NULL;
}

/* Reads exactly n bytes; returns 0, or -1 on close/error/quit. */
static int recv_all(conn_t *c, void *buf, size_t n, volatile sig_atomic_t *quit)
{
    size_t got = 0;
    while (got < n) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        int r = poll(&p, 1, 200);
        if (*quit || !atomic_load(&c->alive))
            return -1;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        ssize_t k = recv(c->fd, (char *)buf + got, n - got, 0);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)k;
    }
    return 0;
}

static void serve_client(app_t *app, int fd, volatile sig_atomic_t *quit)
{
    conn_t c = { .fd = fd, .app = app };
    pthread_mutex_init(&c.send_lock, NULL);
    atomic_init(&c.alive, 1);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    pthread_t th;
    if (pthread_create(&th, NULL, sender_thread, &c)) {
        close(fd);
        return;
    }

    char    *payload = malloc(PROTO_MAX_CMD + 1);
    char    *reply = malloc(16384);
    uint8_t  hdr[PROTO_HDR_SIZE];
    while (payload && reply && recv_all(&c, hdr, sizeof(hdr), quit) == 0) {
        uint16_t type;
        uint32_t len;
        if (proto_parse_hdr(hdr, &type, &len) || len > PROTO_MAX_CMD) {
            fprintf(stderr, "client: protocol error, closing\n");
            break;
        }
        if (len && recv_all(&c, payload, len, quit))
            break;
        payload[len] = 0;
        if (type != MSG_CMD)
            continue;                          /* ignore anything else from the client */
        app_command(app, payload, reply, 16384);
        if (send_msg(&c, MSG_REPLY, reply, strlen(reply)))
            break;
    }
    free(payload);
    free(reply);

    atomic_store(&c.alive, 0);
    shutdown(fd, SHUT_RDWR);
    pthread_join(th, NULL);
    app_stop(app);                             /* rule: a disconnect stops the scan */
    close(fd);
    free(c.enc);
    pthread_mutex_destroy(&c.send_lock);
}

int server_run(app_t *app, int port, volatile sig_atomic_t *quit)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
        return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 1)) {
        perror("bind/listen");
        close(ls);
        return -1;
    }
    printf("listening on port %d\n", port);
    fflush(stdout);

    while (!*quit) {
        struct pollfd p = { .fd = ls, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(ls, (struct sockaddr *)&peer, &plen);
        if (fd < 0)
            continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        printf("client %s:%u connected\n", ip, ntohs(peer.sin_port));
        fflush(stdout);
        serve_client(app, fd, quit);
        printf("client %s disconnected\n", ip);
        fflush(stdout);
    }
    close(ls);
    return 0;
}
