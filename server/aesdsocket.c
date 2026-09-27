/*
 * aesdsocket.c - Assignment 5, AESD
 *
 * A stream socket server listening on TCP port 9000. Every newline-terminated
 * packet received from a client is appended to DATA_FILE, and after each
 * complete packet the full contents of DATA_FILE are sent back to that client.
 * Connections are handled one at a time.
 *
 * Usage: aesdsocket [-d]
 *   -d  run as a daemon. The socket is bound and listening before the fork,
 *       so the port is ready by the time the launching process returns.
 *
 * SIGINT and SIGTERM request a graceful exit: the current connection is
 * closed, the listening socket is closed and DATA_FILE is removed.
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
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define DATA_FILE_MODE 0644
#define LISTEN_BACKLOG 10
#define RX_CHUNK_SIZE 1024
#define FILE_CHUNK_SIZE 1024
#define PACKET_MIN_CAPACITY 1024

/* Set only by the signal handler; polled by the main and connection loops. */
static volatile sig_atomic_t exit_requested = 0;

static void handle_signal(int signo)
{
    (void)signo;
    exit_requested = 1;
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
 * Receive packets from clientfd until the peer closes, an error occurs or an
 * exit is requested. Each newline-terminated packet is appended to DATA_FILE
 * and followed by sending the whole file back. A trailing partial packet at
 * peer close is discarded. The packet buffer is freed on every return path.
 */
static void handle_connection(int clientfd)
{
    char rx[RX_CHUNK_SIZE];
    char *pkt = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool discarding = false;

    while (!exit_requested) {
        ssize_t n = recv(clientfd, rx, sizeof(rx), 0);
        size_t off = 0;

        if (n == 0) {
            break;
        }
        if (n == -1) {
            if (errno == EINTR) {
                continue; /* loop condition re-tests exit_requested */
            }
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        while (off < (size_t)n) {
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
                int rc = append_packet(pkt, len);
                len = 0;
                if (rc != 0 || send_data_file(clientfd) != 0) {
                    free(pkt);
                    return;
                }
            }
        }
    }
    free(pkt);
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

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int opt;
    int sockfd;
    int reuse = 1;
    struct sockaddr_in addr;

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

    while (!exit_requested) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        char client_ip[INET_ADDRSTRLEN];
        int clientfd;

        clientfd = accept(sockfd, (struct sockaddr *)&client, &client_len);
        if (clientfd == -1) {
            if (errno != EINTR) {
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            }
            continue; /* loop condition re-tests exit_requested */
        }

        if (inet_ntop(AF_INET, &client.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            syslog(LOG_ERR, "inet_ntop failed: %s", strerror(errno));
            (void)snprintf(client_ip, sizeof(client_ip), "unknown");
        }
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_connection(clientfd);

        if (close(clientfd) != 0) {
            syslog(LOG_ERR, "close client socket failed: %s", strerror(errno));
        }
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    close_listener(sockfd);
    if (unlink(DATA_FILE) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink %s failed: %s", DATA_FILE, strerror(errno));
    }
    closelog();
    return 0;
}
