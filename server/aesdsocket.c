/*
 * aesdsocket.c - Assignment 6, AESD
 *
 * A multithreaded stream socket server listening on TCP port 9000. Every
 * newline-terminated packet received from a client is appended to DATA_FILE,
 * and after each complete packet the full contents of DATA_FILE are sent back
 * to that client. Each accepted connection is served by its own thread, so any
 * number of clients can be connected at once.
 *
 * Usage: aesdsocket [-d]
 *   -d  run as a daemon. The socket is bound and listening before the fork,
 *       so the port is ready by the time the launching process returns.
 *
 * Locking: data_mutex is the single lock guarding every open/write/read of
 * DATA_FILE. A client's append, read-back and send happen under it, so every
 * client sees a consistent snapshot and packets from different clients are
 * never interleaved. node_mutex guards only the client_fd and thread_complete
 * fields of the per-connection bookkeeping and is never held across blocking
 * I/O or together with data_mutex.
 *
 * Timestamps: a separate thread appends a "timestamp:<RFC 2822 time>" line to
 * DATA_FILE every 10 seconds, measured on CLOCK_MONOTONIC from a fixed
 * deadline so the interval does not drift. The first stamp is written 10
 * seconds after start-up.
 *
 * Shutdown: SIGINT and SIGTERM request a graceful exit. The signal handler
 * shuts down the listening socket so accept() cannot miss the request. Only
 * the main thread handles signals (worker and timestamp threads start with
 * them blocked). On exit the main thread closes the listener, shuts down every
 * client socket so blocked threads wake up, stops the timestamp thread, joins
 * every thread and only then removes DATA_FILE.
 *
 * DATA_FILE is never read into memory as a whole; it is streamed back to the
 * client in fixed-size chunks. Only the packet currently being received is
 * kept on the heap, and a packet too large for available memory is discarded
 * (up to its terminating newline) without closing the connection.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/*
 * glibc's <sys/queue.h> lacks SLIST_FOREACH_SAFE. This is the FreeBSD
 * definition; it caches the next element so the current one may be removed.
 */
#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)            \
    for ((var) = SLIST_FIRST((head));                         \
         (var) && ((tvar) = SLIST_NEXT((var), field), 1);     \
         (var) = (tvar))
#endif

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define DATA_FILE_MODE 0644
#define LISTEN_BACKLOG 10
#define RX_CHUNK_SIZE 1024
#define FILE_CHUNK_SIZE 1024
#define PACKET_MIN_CAPACITY 1024
#define TIMESTAMP_INTERVAL_SEC 10
#define TIMESTAMP_FORMAT "%a, %d %b %Y %H:%M:%S %z"
#define TIMESTAMP_PREFIX "timestamp:"
#define TIMESTAMP_LINE_SIZE 128

/* Per-connection bookkeeping; the list is touched only by the main thread. */
struct thread_node {
    pthread_t thread;
    int client_fd;
    bool thread_complete;
    char client_ip[INET_ADDRSTRLEN];
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list, thread_node);

/* Guards every open/write/read of DATA_FILE. */
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Guards only node->client_fd and node->thread_complete. Never nested. */
static pthread_mutex_t node_mutex = PTHREAD_MUTEX_INITIALIZER;
/* ts_mutex, ts_cond and ts_stop only exist to wake the timestamp thread. */
static pthread_mutex_t ts_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ts_cond;
static pthread_condattr_t ts_condattr;
static bool ts_stop = false;

/* Set only by the signal handler; polled by the main and connection loops. */
static volatile sig_atomic_t exit_requested = 0;
/* Listening socket for the signal handler to shut down, or -1. */
static volatile sig_atomic_t signal_listen_fd = -1;

/*
 * Request exit and shut down the listening socket. shutdown() is
 * async-signal-safe (POSIX.1-2008) and makes a blocked, or about to be
 * entered, accept() fail. That closes the race between the
 * "while (!exit_requested)" check and accept().
 */
static void handle_signal(int signo)
{
    int saved_errno = errno;

    (void)signo;
    exit_requested = 1;
    if (signal_listen_fd >= 0) {
        (void)shutdown(signal_listen_fd, SHUT_RDWR);
    }
    errno = saved_errno;
}
/*
 * Install handle_signal for SIGINT and SIGTERM. SA_RESTART is deliberately
 * not set so that a blocking accept()/recv() returns EINTR on a signal.
 * Returns 0 on success, -1 on failure (already logged).
 */
static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) != 0) {
        syslog(LOG_ERR, "sigemptyset failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Write all len bytes of buf to fd, retrying on partial writes and EINTR.
 * Returns 0 on success, -1 on failure (already logged).
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "write to %s failed: %s", DATA_FILE, strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Send all len bytes of buf on sockfd, retrying on partial sends. EINTR is
 * retried unless an exit has been requested. MSG_NOSIGNAL keeps a vanished
 * peer from raising SIGPIPE. Returns 0 on success, -1 on failure.
 */
static int send_all(int sockfd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(sockfd, buf + done, len - done, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR) {
                if (exit_requested) {
                    return -1;
                }
                continue;
            }
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Append one complete packet to DATA_FILE.
 * Returns 0 on success, -1 on failure (already logged).
 */
static int append_packet(const char *pkt, size_t len)
{
    int fd;
    int rc;

    fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, DATA_FILE_MODE);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for append failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    rc = write_all(fd, pkt, len);
    if (close(fd) != 0) {
        syslog(LOG_ERR, "close %s after append failed: %s", DATA_FILE, strerror(errno));
        rc = -1;
    }
    return rc;
}

/*
 * Stream the whole of DATA_FILE to sockfd in FILE_CHUNK_SIZE pieces.
 * Returns 0 on success, -1 on failure.
 */
static int send_data_file(int sockfd)
{
    char chunk[FILE_CHUNK_SIZE];
    int fd;
    int rc = 0;

    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for read failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n == 0) {
            break;
        }
        if (n == -1) {
            if (errno == EINTR && !exit_requested) {
                continue;
            }
            if (errno != EINTR) {
                syslog(LOG_ERR, "read %s failed: %s", DATA_FILE, strerror(errno));
            }
            rc = -1;
            break;
        }
        if (send_all(sockfd, chunk, (size_t)n) != 0) {
            rc = -1;
            break;
        }
    }
    if (close(fd) != 0) {
        syslog(LOG_ERR, "close %s after read failed: %s", DATA_FILE, strerror(errno));
        rc = -1;
    }
    return rc;
}

/*
 * Append n bytes of data to the heap packet buffer, growing it by doubling
 * (minimum PACKET_MIN_CAPACITY). Returns 0 on success, -1 if the buffer could
 * not be grown; in that case the buffer is left unchanged.
 */
static int packet_append(char **pkt, size_t *len, size_t *cap, const char *data, size_t n)
{
    if (n > SIZE_MAX - *len) {
        return -1;
    }
    if (*len + n > *cap) {
        size_t need = *len + n;
        size_t new_cap = (*cap != 0) ? *cap : PACKET_MIN_CAPACITY;
        char *grown;

        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = need;
                break;
            }
            new_cap *= 2;
        }
        grown = realloc(*pkt, new_cap);
        if (grown == NULL) {
            return -1;
        }
        *pkt = grown;
        *cap = new_cap;
    }
    memcpy(*pkt + *len, data, n);
    *len += n;
    return 0;
}

/*
 * Lock m, logging a failure. Returns 0 on success, -1 on failure.
 */
static int lock_mutex(pthread_mutex_t *m)
{
    int rc = pthread_mutex_lock(m);

    if (rc != 0) {
        syslog(LOG_ERR, "pthread_mutex_lock failed: %s", strerror(rc));
        return -1;
    }
    return 0;
}

/*
 * Unlock m, logging a failure. Returns 0 on success, -1 on failure.
 */
static int unlock_mutex(pthread_mutex_t *m)
{
    int rc = pthread_mutex_unlock(m);

    if (rc != 0) {
        syslog(LOG_ERR, "pthread_mutex_unlock failed: %s", strerror(rc));
        return -1;
    }
    return 0;
}

/*
 * Start a thread with SIGINT and SIGTERM blocked, so the new thread inherits
 * the blocked mask and only the main thread ever runs the signal handler. The
 * caller's mask is restored afterwards. Returns 0 on success, -1 on failure
 * (already logged). If only the final mask restore fails the thread is
 * running, so that is logged and 0 is still returned.
 */
static int create_thread(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    sigset_t block;
    sigset_t old;
    int rc;

    if (sigemptyset(&block) != 0 || sigaddset(&block, SIGINT) != 0 ||
        sigaddset(&block, SIGTERM) != 0) {
        syslog(LOG_ERR, "building signal set failed: %s", strerror(errno));
        return -1;
    }
    rc = pthread_sigmask(SIG_BLOCK, &block, &old);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_sigmask block failed: %s", strerror(rc));
        return -1;
    }
    rc = pthread_create(thread, NULL, fn, arg);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
    }
    {
        int mask_rc = pthread_sigmask(SIG_SETMASK, &old, NULL);

        if (mask_rc != 0) {
            syslog(LOG_ERR, "pthread_sigmask restore failed: %s", strerror(mask_rc));
        }
    }
    return (rc == 0) ? 0 : -1;
}

/*
 * Append one packet to DATA_FILE and send the whole file back to sockfd, all
 * under data_mutex so the client gets a consistent snapshot. The mutex is
 * released on every path. Returns 0 on success, -1 on failure (logged).
 */
static int process_packet(int sockfd, const char *pkt, size_t len)
{
    int rc;

    if (lock_mutex(&data_mutex) != 0) {
        return -1;
    }
    rc = append_packet(pkt, len);
    if (rc == 0) {
        rc = send_data_file(sockfd);
    }
    if (unlock_mutex(&data_mutex) != 0) {
        rc = -1;
    }
    return rc;
}

/*
 * Connection thread: receive packets from the client until the peer closes or
 * an error occurs. Each newline-terminated packet is appended to DATA_FILE and
 * followed by sending the whole file back. A trailing partial packet at peer
 * close is discarded. The packet buffer is freed on every path. On the way out
 * the socket is closed and the node is marked complete for the main thread to
 * join. The thread never touches the list itself. Returns NULL.
 */
static void *connection_thread(void *arg)
{
    struct thread_node *node = arg;
    int clientfd = node->client_fd;
    char rx[RX_CHUNK_SIZE];
    char *pkt = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool discarding = false;
    bool done = false;

    while (!done) {
        ssize_t n = recv(clientfd, rx, sizeof(rx), 0);
        size_t off = 0;

        if (n == 0) {
            break;
        }
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        while (off < (size_t)n && !done) {
            char *nl = memchr(rx + off, '\n', (size_t)n - off);
            size_t seg = (nl != NULL) ? (size_t)(nl - (rx + off)) + 1 : (size_t)n - off;

            if (discarding) {
                /* Drop bytes of an oversized packet up to its newline. */
                if (nl != NULL) {
                    discarding = false;
                }
                off += seg;
                continue;
            }

            if (packet_append(&pkt, &len, &cap, rx + off, seg) != 0) {
                syslog(LOG_ERR, "packet too large for available memory, discarding");
                free(pkt);
                pkt = NULL;
                len = 0;
                cap = 0;
                /* A newline in this segment already ends the discarded packet. */
                discarding = (nl == NULL);
                off += seg;
                continue;
            }
            off += seg;

            if (nl != NULL) {
                int rc = process_packet(clientfd, pkt, len);

                len = 0;
                if (rc != 0) {
                    done = true;
                }
            }
        }
    }
    free(pkt);
    syslog(LOG_INFO, "Closed connection from %s", node->client_ip);

    if (lock_mutex(&node_mutex) == 0) {
        if (close(clientfd) != 0) {
            syslog(LOG_ERR, "close client socket failed: %s", strerror(errno));
        }
        node->client_fd = -1;
        node->thread_complete = true;
        (void)unlock_mutex(&node_mutex);
    }
    return NULL;
}

/*
 * Append one "timestamp:<time>" line to DATA_FILE under data_mutex.
 * Returns 0 on success, -1 on failure (already logged).
 */
static int write_timestamp(void)
{
    char stamp[TIMESTAMP_LINE_SIZE];
    char timestr[TIMESTAMP_LINE_SIZE];
    struct tm tm_now;
    time_t now;
    int len;
    int rc;

    now = time(NULL);
    if (now == (time_t)-1) {
        syslog(LOG_ERR, "time failed: %s", strerror(errno));
        return -1;
    }
    if (localtime_r(&now, &tm_now) == NULL) {
        syslog(LOG_ERR, "localtime_r failed");
        return -1;
    }
    if (strftime(timestr, sizeof(timestr), TIMESTAMP_FORMAT, &tm_now) == 0) {
        syslog(LOG_ERR, "strftime failed");
        return -1;
    }
    len = snprintf(stamp, sizeof(stamp), TIMESTAMP_PREFIX "%s\n", timestr);
    if (len < 0 || (size_t)len >= sizeof(stamp)) {
        syslog(LOG_ERR, "timestamp line truncated");
        return -1;
    }

    if (lock_mutex(&data_mutex) != 0) {
        return -1;
    }
    rc = append_packet(stamp, (size_t)len);
    if (unlock_mutex(&data_mutex) != 0) {
        rc = -1;
    }
    return rc;
}

/*
 * Timestamp thread: every TIMESTAMP_INTERVAL_SEC seconds append a timestamp
 * line to DATA_FILE. The deadline advances by a fixed interval from the
 * previous deadline on CLOCK_MONOTONIC, so there is no drift. Returns NULL
 * when ts_stop is set or a wait fails, with ts_mutex unlocked.
 */
static void *timestamp_thread(void *arg)
{
    struct timespec deadline;
    bool failed = false;

    (void)arg;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        syslog(LOG_ERR, "clock_gettime failed: %s", strerror(errno));
        return NULL;
    }
    if (lock_mutex(&ts_mutex) != 0) {
        return NULL;
    }
    while (!ts_stop && !failed) {
        int rc = 0;

        deadline.tv_sec += TIMESTAMP_INTERVAL_SEC;
        while (!ts_stop) {
            rc = pthread_cond_timedwait(&ts_cond, &ts_mutex, &deadline);
            if (rc == ETIMEDOUT) {
                break;
            }
            if (rc != 0) {
                syslog(LOG_ERR, "pthread_cond_timedwait failed: %s", strerror(rc));
                failed = true;
                break;
            }
        }
        if (ts_stop || failed) {
            break;
        }

        (void)unlock_mutex(&ts_mutex);
        (void)write_timestamp();
        if (lock_mutex(&ts_mutex) != 0) {
            return NULL;
        }
    }
    (void)unlock_mutex(&ts_mutex);
    return NULL;
}

/*
 * Join and free every completed node. A node that is not yet complete is never
 * joined here. Main thread only. No return value; failures are logged.
 */
static void reap_completed_threads(struct thread_list *list)
{
    struct thread_node *node;
    struct thread_node *next;

    SLIST_FOREACH_SAFE(node, list, entries, next) {
        bool complete = false;
        int rc;

        if (lock_mutex(&node_mutex) != 0) {
            continue;
        }
        complete = node->thread_complete;
        (void)unlock_mutex(&node_mutex);
        if (!complete) {
            continue;
        }
        rc = pthread_join(node->thread, NULL);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_join failed: %s", strerror(rc));
        }
        SLIST_REMOVE(list, node, thread_node, entries);
        free(node);
    }
}

/*
 * Detach from the controlling terminal: fork (parent exits), start a new
 * session, chdir to / and point stdin/stdout/stderr at /dev/null.
 * Returns 0 in the daemon child, -1 on failure (already logged). Never returns
 * in the parent.
 */
static int daemonize(void)
{
    pid_t pid;
    int nullfd;
    int fd;

    pid = fork();
    if (pid == -1) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid != 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }
    if (chdir("/") != 0) {
        syslog(LOG_ERR, "chdir / failed: %s", strerror(errno));
        return -1;
    }
    nullfd = open("/dev/null", O_RDWR);
    if (nullfd == -1) {
        syslog(LOG_ERR, "open /dev/null failed: %s", strerror(errno));
        return -1;
    }
    for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (dup2(nullfd, fd) == -1) {
            syslog(LOG_ERR, "dup2 /dev/null onto fd %d failed: %s", fd, strerror(errno));
            if (nullfd > STDERR_FILENO) {
                (void)close(nullfd);
            }
            return -1;
        }
    }
    if (nullfd > STDERR_FILENO && close(nullfd) != 0) {
        syslog(LOG_ERR, "close /dev/null failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Close the listening socket, logging any failure. */
static void close_listener(int sockfd)
{
    if (close(sockfd) != 0) {
        syslog(LOG_ERR, "close listening socket failed: %s", strerror(errno));
    }
}

/*
 * Initialise ts_cond to wait on CLOCK_MONOTONIC (ts_condattr is kept for
 * destruction at exit). Returns 0 on success, -1 on failure (already logged).
 */
static int init_timestamp_cond(void)
{
    int rc = pthread_condattr_init(&ts_condattr);

    if (rc != 0) {
        syslog(LOG_ERR, "pthread_condattr_init failed: %s", strerror(rc));
        return -1;
    }
    rc = pthread_condattr_setclock(&ts_condattr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(&ts_cond, &ts_condattr);
    }
    if (rc != 0) {
        syslog(LOG_ERR, "timestamp condition setup failed: %s", strerror(rc));
        (void)pthread_condattr_destroy(&ts_condattr);
        return -1;
    }
    return 0;
}

/* Destroy the condition variable, its attributes and the mutexes. */
static void destroy_sync_objects(void)
{
    (void)pthread_cond_destroy(&ts_cond);
    (void)pthread_condattr_destroy(&ts_condattr);
    (void)pthread_mutex_destroy(&ts_mutex);
    (void)pthread_mutex_destroy(&node_mutex);
    (void)pthread_mutex_destroy(&data_mutex);
}

/*
 * Shut down every still-open client socket so blocked recv()/send() calls in
 * connection threads fail and the threads finish.
 */
static void shutdown_clients(struct thread_list *list)
{
    struct thread_node *node;

    if (lock_mutex(&node_mutex) != 0) {
        return;
    }
    SLIST_FOREACH(node, list, entries) {
        if (node->client_fd >= 0 && shutdown(node->client_fd, SHUT_RDWR) != 0) {
            syslog(LOG_ERR, "shutdown client socket failed: %s", strerror(errno));
        }
    }
    (void)unlock_mutex(&node_mutex);
}

/* Tell the timestamp thread to stop and join it. Failures are logged. */
static void stop_timestamp_thread(pthread_t thread)
{
    int rc;

    if (lock_mutex(&ts_mutex) == 0) {
        ts_stop = true;
        rc = pthread_cond_signal(&ts_cond);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_cond_signal failed: %s", strerror(rc));
        }
        (void)unlock_mutex(&ts_mutex);
    }
    rc = pthread_join(thread, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_join timestamp thread failed: %s", strerror(rc));
    }
}

/* Join and free every remaining node. Main thread only. */
static void join_all_threads(struct thread_list *list)
{
    while (!SLIST_EMPTY(list)) {
        struct thread_node *node = SLIST_FIRST(list);
        int rc = pthread_join(node->thread, NULL);

        if (rc != 0) {
            syslog(LOG_ERR, "pthread_join failed: %s", strerror(rc));
        }
        SLIST_REMOVE_HEAD(list, entries);
        free(node);
    }
}

/*
 * Block SIGINT and SIGTERM in the calling (main) thread so no late signal can
 * run the handler during cleanup. Returns 0 on success, -1 on failure.
 */
static int block_exit_signals(void)
{
    sigset_t block;
    int rc;

    if (sigemptyset(&block) != 0 || sigaddset(&block, SIGINT) != 0 ||
        sigaddset(&block, SIGTERM) != 0) {
        syslog(LOG_ERR, "building signal set failed: %s", strerror(errno));
        return -1;
    }
    rc = pthread_sigmask(SIG_BLOCK, &block, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_sigmask block failed: %s", strerror(rc));
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int opt;
    int sockfd;
    int reuse = 1;
    struct sockaddr_in addr;
    struct thread_list list = SLIST_HEAD_INITIALIZER(list);
    pthread_t ts_thread;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = true;
            break;
        default:
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            closelog();
            return -1;
        }
    }

    if (install_signal_handlers() != 0) {
        closelog();
        return -1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        closelog();
        return -1;
    }
    signal_listen_fd = sockfd;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        syslog(LOG_ERR, "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close_listener(sockfd);
        closelog();
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        syslog(LOG_ERR, "bind to port %d failed: %s", PORT, strerror(errno));
        close_listener(sockfd);
        closelog();
        return -1;
    }
    if (listen(sockfd, LISTEN_BACKLOG) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close_listener(sockfd);
        closelog();
        return -1;
    }

    /* Only daemonize once the port is listening. */
    if (daemon_mode && daemonize() != 0) {
        close_listener(sockfd);
        closelog();
        return -1;
    }

    if (init_timestamp_cond() != 0) {
        close_listener(sockfd);
        closelog();
        return -1;
    }
    if (create_thread(&ts_thread, timestamp_thread, NULL) != 0) {
        close_listener(sockfd);
        (void)pthread_cond_destroy(&ts_cond);
        (void)pthread_condattr_destroy(&ts_condattr);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        struct thread_node *node;
        int clientfd;

        clientfd = accept(sockfd, (struct sockaddr *)&client, &client_len);
        if (clientfd == -1) {
            if (exit_requested) {
                break;
            }
            if (errno != EINTR) {
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            }
            continue;
        }

        node = calloc(1, sizeof(*node));
        if (node == NULL) {
            syslog(LOG_ERR, "calloc failed for connection node");
            if (close(clientfd) != 0) {
                syslog(LOG_ERR, "close client socket failed: %s", strerror(errno));
            }
            continue;
        }
        node->client_fd = clientfd;
        node->thread_complete = false;
        if (inet_ntop(AF_INET, &client.sin_addr, node->client_ip, sizeof(node->client_ip)) ==
            NULL) {
            syslog(LOG_ERR, "inet_ntop failed: %s", strerror(errno));
            (void)snprintf(node->client_ip, sizeof(node->client_ip), "unknown");
        }
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        SLIST_INSERT_HEAD(&list, node, entries);
        if (create_thread(&node->thread, connection_thread, node) != 0) {
            SLIST_REMOVE(&list, node, thread_node, entries);
            if (close(clientfd) != 0) {
                syslog(LOG_ERR, "close client socket failed: %s", strerror(errno));
            }
            free(node);
            continue;
        }

        reap_completed_threads(&list);
    }

    /* No signal may run the handler from here on; the listener goes away. */
    (void)block_exit_signals();
    signal_listen_fd = -1;
    syslog(LOG_INFO, "Caught signal, exiting");
    close_listener(sockfd);
    shutdown_clients(&list);
    stop_timestamp_thread(ts_thread);
    join_all_threads(&list);
    /* Every thread is joined, so nothing can recreate the file. */
    if (unlink(DATA_FILE) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink %s failed: %s", DATA_FILE, strerror(errno));
    }
    destroy_sync_objects();
    closelog();
    return 0;
}
