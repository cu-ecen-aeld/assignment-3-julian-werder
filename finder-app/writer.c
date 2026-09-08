/*
 * writer.c - Assignment 2, AESD
 *
 * A native/cross-compilable replacement for the assignment 1 "writer.sh"
 * script.  Writes <writestr> into <writefile>, creating or truncating that
 * file, and logs what it did to syslog using the LOG_USER facility.
 *
 * Unlike writer.sh this utility does NOT create missing directories; the
 * caller is responsible for that (see the assignment 2 instructions).
 *
 * File I/O uses the open/write/close system calls described in Linux System
 * Programming chapter 2 rather than the stdio stream interface.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *writefile;
    const char *writestr;
    size_t to_write;
    size_t written = 0;
    int fd;

    /* LOG_USER facility, as required.  LOG_PID makes concurrent runs (the
     * finder-test.sh loop) distinguishable in /var/log/syslog. */
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        fprintf(stderr, "  <writefile>  full path to the file to write\n");
        fprintf(stderr, "  <writestr>   text string to write into that file\n");
        syslog(LOG_ERR, "Invalid argument count: expected 2 arguments, got %d", argc - 1);
        closelog();
        return 1;
    }

    writefile = argv[1];
    writestr = argv[2];

    /* Required message, at LOG_DEBUG level. */
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        fprintf(stderr, "Error: could not open %s for writing: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Could not open %s for writing: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    /* write() may return a short count or be interrupted by a signal, so loop
     * until the whole string has landed rather than assuming one call
     * suffices (LSP ch. 2, "Partial Writes"). */
    to_write = strlen(writestr);
    while (written < to_write)
    {
        ssize_t ret = write(fd, writestr + written, to_write - written);

        if (ret == -1)
        {
            if (errno == EINTR)
            {
                continue; /* interrupted before writing anything; retry */
            }
            fprintf(stderr, "Error: write to %s failed: %s\n", writefile, strerror(errno));
            syslog(LOG_ERR, "Error writing to %s: %s", writefile, strerror(errno));
            close(fd);
            closelog();
            return 1;
        }

        written += (size_t)ret;
    }

    /* close() can fail (e.g. a deferred write-back error on NFS); reporting
     * success without checking it would hide a genuinely lost write. */
    if (close(fd) == -1)
    {
        fprintf(stderr, "Error: closing %s failed: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Error closing %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
