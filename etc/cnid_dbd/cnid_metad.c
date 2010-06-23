/*
 * $Id: cnid_metad.c,v 1.22 2009-11-16 02:04:47 didg Exp $
 *
 * Copyright (C) Joerg Lenneis 2003
 * All Rights Reserved.  See COPYING.
 *
 */

/* 
   cnid_dbd metadaemon to start up cnid_dbd upon request from afpd.
   Here is how it works:
   
                       via TCP socket
   1.       afpd          ------->        cnid_metad

                   via UNIX domain socket
   2.   cnid_metad        ------->         cnid_dbd

                    passes afpd client fd
   3.   cnid_metad        ------->         cnid_dbd

   Result:
                       via TCP socket
   4.       afpd          ------->         cnid_dbd
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#undef __USE_GNU

#include <stdlib.h>
#include <sys/param.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#include <sys/un.h>
#define _XPG4_2 1
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>
#include <sys/ioctl.h>

#ifndef WEXITSTATUS
#define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
#endif /* ! WEXITSTATUS */
#ifndef WIFEXITED
#define WIFEXITED(stat_val) (((stat_val) & 255) == 0)
#endif /* ! WIFEXITED */
#ifndef WIFSTOPPED
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#endif

#ifndef WIFSIGNALED
#define WIFSIGNALED(status) (!WIFSTOPPED(status) && !WIFEXITED(status))
#endif
#ifndef WTERMSIG
#define WTERMSIG(status)      ((status) & 0x7f)
#endif

/* functions for username and group */
#include <pwd.h>
#include <grp.h>

/* FIXME */
#ifdef linux
#ifndef USE_SETRESUID
#define USE_SETRESUID 1
#define SWITCH_TO_GID(gid)  ((setresgid(gid,gid,gid) < 0 || setgid(gid) < 0) ? -1 : 0)
#define SWITCH_TO_UID(uid)  ((setresuid(uid,uid,uid) < 0 || setuid(uid) < 0) ? -1 : 0)
#endif  /* USE_SETRESUID */
#else   /* ! linux */
#ifndef USE_SETEUID
#define USE_SETEUID 1
#define SWITCH_TO_GID(gid)  ((setegid(gid) < 0 || setgid(gid) < 0) ? -1 : 0)
#define SWITCH_TO_UID(uid)  ((setuid(uid) < 0 || seteuid(uid) < 0 || setuid(uid) < 0) ? -1 : 0)
#endif  /* USE_SETEUID */
#endif  /* linux */

#include <atalk/util.h>
#include <atalk/logger.h>
#include <atalk/cnid_dbd_private.h>

#include "db_param.h"
#include "usockfd.h"

#define DBHOME        ".AppleDB"
#define DBHOMELEN    8

static int srvfd;
static int rqstfd;
static volatile sig_atomic_t sigchild = 0;

#define MAXSPAWN   3                   /* Max times respawned in.. */
#define TESTTIME   42                  /* this much seconds apfd client tries to  *
                                        * to reconnect every 5 secondes, catch it */
#define MAXVOLS    512
#define DEFAULTHOST  "localhost"
#define DEFAULTPORT  "4700"

struct server {
    char  *name;
    pid_t pid;
    time_t tm;                    /* When respawned last */
    int count;                    /* Times respawned in the last TESTTIME secondes */
    int control_fd;               /* file descriptor to child cnid_dbd process */
};

static struct server srv[MAXVOLS];

/* Default logging config: log to syslog with level log_note */
static char logconfig[MAXPATHLEN + 21 + 1] = "default log_note";

static struct server *test_usockfn(char *dir)
{
    int i;
    for (i = 0; i < MAXVOLS; i++) {
        if (srv[i].name && !strcmp(srv[i].name, dir)) {
            return &srv[i];
        }
    }
    return NULL;
}

/* -------------------- */
static int send_cred(int socket, int fd)
{
    int ret;
    struct msghdr msgh;
    struct iovec iov[1];
    struct cmsghdr *cmsgp = NULL;
    char *buf;
    size_t size;
    int er=0;

    size = CMSG_SPACE(sizeof fd);
    buf = malloc(size);
    if (!buf) {
        LOG(log_error, logtype_cnid, "error in sendmsg: %s", strerror(errno));
        return -1;
    }

    memset(&msgh,0,sizeof (msgh));
    memset(buf,0, size);

    msgh.msg_name = NULL;
    msgh.msg_namelen = 0;

    msgh.msg_iov = iov;
    msgh.msg_iovlen = 1;

    iov[0].iov_base = &er;
    iov[0].iov_len = sizeof(er);

    msgh.msg_control = buf;
    msgh.msg_controllen = size;

    cmsgp = CMSG_FIRSTHDR(&msgh);
    cmsgp->cmsg_level = SOL_SOCKET;
    cmsgp->cmsg_type = SCM_RIGHTS;
    cmsgp->cmsg_len = CMSG_LEN(sizeof(fd));

    *((int *)CMSG_DATA(cmsgp)) = fd;
    msgh.msg_controllen = cmsgp->cmsg_len;

    do  {
        ret = sendmsg(socket,&msgh, 0);
    } while ( ret == -1 && errno == EINTR );
    if (ret == -1) {
        LOG(log_error, logtype_cnid, "error in sendmsg: %s", strerror(errno));
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

/* -------------------- */
static int maybe_start_dbd(char *dbdpn, char *dbdir, char *usockfn)
{
    pid_t pid;
    struct server *up;
    int sv[2];
    int i;
    time_t t;
    char buf1[8];
    char buf2[8];

    LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: dbdir: '%s', UNIX socket file: '%s'", 
        dbdir, usockfn);

    up = test_usockfn(dbdir);
    if (up && up->pid) {
        /* we already have a process, send our fd */
        if (send_cred(up->control_fd, rqstfd) < 0) {
            /* FIXME */
            return -1;
        }
        return 0;
    }

    LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: no cnid_dbd for that volume yet. Starting one ...");

    time(&t);
    if (!up) {
        /* find an empty slot */
        for (i = 0; i < MAXVOLS; i++) {
            if ( !srv[i].name ) {
                up = &srv[i];
                up->tm = t;
                up->count = 0;
                up->name = strdup(dbdir);
                break;
            }
        }
        if (!up) {
            LOG(log_error, logtype_cnid, "no free slot for cnid_dbd child. Configured maximum: %d. Do you have so many volumes?", MAXVOLS);
            return -1;
        }
    }
    else {
        /* we have a slot but no process, check for respawn too fast */
        if ( (t < (up->tm + TESTTIME)) /* We're in the respawn time window */
             &&
             (up->count > MAXSPAWN) ) { /* ...and already tried to fork too often */
            LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: respawn too fast just exiting");
            return -1; /* just exit, dont sleep, because we might have work to do for another client  */
        }
        if ( t >= (up->tm + TESTTIME) ) { /* out of respawn too fast windows reset the count */
            LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: respawn window ended");
            up->tm = t;
            up->count = 0;
        }
        up->count++;
        LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: respawn count now is: %u", up->count);
        if (up->count > MAXSPAWN) {
            /* We spawned too fast. From now until the first time we tried + TESTTIME seconds
               we will just return -1 above */
            LOG(log_maxdebug, logtype_cnid, "maybe_start_dbd: reached MAXSPAWN threshhold");
        }
    }

    /* 
       Create socketpair for comm between parent and child.
       We use it to pass fds from connecting afpd processes to our
       cnid_dbd child via fd passing.
    */
    if (socketpair(PF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        LOG(log_error, logtype_cnid, "error in socketpair: %s", strerror(errno));
        return -1;
    }

    if ((pid = fork()) < 0) {
        LOG(log_error, logtype_cnid, "error in fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int ret;
        /*
         *  Child. Close descriptors and start the daemon. If it fails
         *  just log it. The client process will fail connecting
         *  afterwards anyway.
         */

        close(srvfd);
        close(sv[0]);

        for (i = 0; i < MAXVOLS; i++) {
            if (srv[i].pid && up != &srv[i]) {
                close(srv[i].control_fd);
            }
        }

        sprintf(buf1, "%i", sv[1]);
        sprintf(buf2, "%i", rqstfd);

        if (up->count == MAXSPAWN) {
            /* there's a pb with the db inform child
             * it will run recover, delete the db whatever
             */
            LOG(log_error, logtype_cnid, "try with -d %s", up->name);
            ret = execlp(dbdpn, dbdpn, "-d", dbdir, buf1, buf2, logconfig, NULL);
        }
        else {
            ret = execlp(dbdpn, dbdpn, dbdir, buf1, buf2, logconfig, NULL);
        }
        if (ret < 0) {
            LOG(log_error, logtype_cnid, "Fatal error in exec: %s", strerror(errno));
            exit(0);
        }
    }
    /*
     *  Parent.
     */
    up->pid = pid;
    close(sv[1]);
    up->control_fd = sv[0];
    return 0;
}

/* ------------------ */
static int set_dbdir(char *dbdir, int len)
{
    struct stat st;

    if (!len)
        return -1;

    if (stat(dbdir, &st) < 0 && mkdir(dbdir, 0755) < 0) {
        LOG(log_error, logtype_cnid, "set_dbdir: mkdir failed for %s", dbdir);
        return -1;
    }

    if (dbdir[len - 1] != '/') {
        strcat(dbdir, "/");
        len++;
    }
    strcpy(dbdir + len, DBHOME);
    if (stat(dbdir, &st) < 0 && mkdir(dbdir, 0755 ) < 0) {
        LOG(log_error, logtype_cnid, "set_dbdir: mkdir failed for %s", dbdir);
        return -1;
    }
    return 0;
}

/* ------------------ */
static uid_t user_to_uid (char *username)
{
    struct passwd *this_passwd;

    /* check for anything */
    if ( !username || strlen ( username ) < 1 ) return 0;

    /* grab the /etc/passwd record relating to username */
    this_passwd = getpwnam ( username );

    /* return false if there is no structure returned */
    if (this_passwd == NULL) return 0;

    /* return proper uid */
    return this_passwd->pw_uid;

}

/* ------------------ */
static gid_t group_to_gid ( char *group)
{
    struct group *this_group;

    /* check for anything */
    if ( !group || strlen ( group ) < 1 ) return 0;

    /* grab the /etc/groups record relating to group */
    this_group = getgrnam ( group );

    /* return false if there is no structure returned */
    if (this_group == NULL) return 0;

    /* return proper gid */
    return this_group->gr_gid;

}

/* ------------------ */
static void catch_child(int sig _U_) 
{
    sigchild = 1;
}

/* ----------------------- */
static void set_signal(void)
{
    struct sigaction sv;
    sigset_t set;

    signal(SIGPIPE, SIG_IGN);

    sv.sa_handler = catch_child;
    sv.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&sv.sa_mask);
    if (sigaction(SIGCHLD, &sv, NULL) < 0) {
        LOG(log_error, logtype_cnid, "cnid_metad: sigaction: %s", strerror(errno));
        exit(1);
    }
    /* block everywhere but in pselect */
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

/* ------------------ */
int main(int argc, char *argv[])
{
    char  dbdir[MAXPATHLEN + 1];
    int   len, actual_len;
    pid_t pid;
    int   status;
    char  *dbdpn = _PATH_CNID_DBD;
    char  *host = DEFAULTHOST;
    char  *port = DEFAULTPORT;
    struct db_param *dbp;
    int    i;
    int    cc;
    uid_t  uid = 0;
    gid_t  gid = 0;
    int    err = 0;
    int    debug = 0;
    int    ret;
    char   *loglevel = NULL;
    char   *logfile  = NULL;
    sigset_t set;

    set_processname("cnid_metad");

    while (( cc = getopt( argc, argv, "ds:p:h:u:g:l:f:")) != -1 ) {
        switch (cc) {
        case 'd':
            debug = 1;
            break;
        case 'h':
            host = strdup(optarg);
            break;
        case 'u':
            uid = user_to_uid (optarg);
            if (!uid) {
                LOG(log_error, logtype_cnid, "main: bad user %s", optarg);
                err++;
            }
            break;
        case 'g':
            gid =group_to_gid (optarg);
            if (!gid) {
                LOG(log_error, logtype_cnid, "main: bad group %s", optarg);
                err++;
            }
            break;
        case 'p':
            port = strdup(optarg);
            break;
        case 's':
            dbdpn = strdup(optarg);
            break;
        case 'l':
            loglevel = strdup(optarg);
            break;
        case 'f':
            logfile = strdup(optarg);
            break;
        default:
            err++;
            break;
        }
    }

    if (loglevel) {
        strlcpy(logconfig + 8, loglevel, 13);
        free(loglevel);
        strcat(logconfig, " ");
    }
    if (logfile) {
        strlcat(logconfig, logfile, MAXPATHLEN);
        free(logfile);
    }
    setuplog(logconfig);

    if (err) {
        LOG(log_error, logtype_cnid, "main: bad arguments");
        exit(1);
    }

    if (!debug) {

        switch (fork()) {
        case 0 :
            fclose(stdin);
            fclose(stdout);
            fclose(stderr);

#ifdef TIOCNOTTY
            {
                int i;
                if (( i = open( "/dev/tty", O_RDWR )) >= 0 ) {
                    (void)ioctl( i, TIOCNOTTY, 0 );
                    setpgid( 0, getpid());
                    (void) close(i);
                }
            }
#else
            setpgid( 0, getpid());
#endif
            break;
        case -1 :  /* error */
            LOG(log_error, logtype_cnid, "detach from terminal: %s", strerror(errno));
            exit(1);
        default :  /* server */
            exit(0);
        }
    }

    if ((srvfd = tsockfd_create(host, port, 10)) < 0)
        exit(1);

    /* switch uid/gid */
    if (uid || gid) {
        LOG(log_info, logtype_cnid, "Setting uid/gid to %i/%i", uid, gid);
        if (gid) {
            if (SWITCH_TO_GID(gid) < 0) {
                LOG(log_info, logtype_cnid, "unable to switch to group %d", gid);
                exit(1);
            }
        }
        if (uid) {
            if (SWITCH_TO_UID(uid) < 0) {
                LOG(log_info, logtype_cnid, "unable to switch to user %d", uid);
                exit(1);
            }
        }
    }

    set_signal();

    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, NULL, &set);
    sigdelset(&set, SIGCHLD);

    while (1) {
        rqstfd = usockfd_check(srvfd, &set);
        /* Collect zombie processes and log what happened to them */
        if (sigchild) while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (i = 0; i < MAXVOLS; i++) {
                if (srv[i].pid == pid) {
                    srv[i].pid = 0;
                    close(srv[i].control_fd);
                    break;
                }
            }
            if (WIFEXITED(status)) {
                LOG(log_info, logtype_cnid, "cnid_dbd pid %i exited with exit code %i",
                    pid, WEXITSTATUS(status));
            }
            else if (WIFSIGNALED(status)) {
                LOG(log_info, logtype_cnid, "cnid_dbd pid %i exited with signal %i",
                    pid, WTERMSIG(status));
            }
            sigchild = 0;
        }
        if (rqstfd <= 0)
            continue;

        /* TODO: Check out read errors, broken pipe etc. in libatalk. Is
           SIGIPE ignored there? Answer: Ignored for dsi, but not for asp ... */
        ret = read(rqstfd, &len, sizeof(int));
        if (!ret) {
            /* already close */
            goto loop_end;
        }
        else if (ret < 0) {
            LOG(log_severe, logtype_cnid, "error read: %s", strerror(errno));
            goto loop_end;
        }
        else if (ret != sizeof(int)) {
            LOG(log_error, logtype_cnid, "short read: got %d", ret);
            goto loop_end;
        }
        /*
         *  checks for buffer overruns. The client libatalk side does it too
         *  before handing the dir path over but who trusts clients?
         */
        if (!len || len +DBHOMELEN +2 > MAXPATHLEN) {
            LOG(log_error, logtype_cnid, "wrong len parameter: %d", len);
            goto loop_end;
        }

        actual_len = read(rqstfd, dbdir, len);
        if (actual_len < 0) {
            LOG(log_severe, logtype_cnid, "Read(2) error : %s", strerror(errno));
            goto loop_end;
        }
        if (actual_len != len) {
            LOG(log_error, logtype_cnid, "error/short read (dir): %s", strerror(errno));
            goto loop_end;
        }
        dbdir[len] = '\0';

        if (set_dbdir(dbdir, len) < 0) {
            goto loop_end;
        }

        if ((dbp = db_param_read(dbdir, METAD)) == NULL) {
            LOG(log_error, logtype_cnid, "Error reading config file");
            goto loop_end;
        }
        maybe_start_dbd(dbdpn, dbdir, dbp->usock_file);

    loop_end:
        close(rqstfd);
    }
}
