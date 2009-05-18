/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <dlezcano at fr.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "../config.h"
#include <stdio.h>
#undef _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/poll.h>

#ifdef HAVE_SYS_SIGNALFD_H 
#  include <sys/signalfd.h>
#else
#  ifndef __NR_signalfd4
/* assume kernel headers are too old */
#    if __i386__
#      define __NR_signalfd4 327
#    elif __x86_64__
#      define __NR_signalfd4 289
#    elif __powerpc__
#      define __NR_signalfd4 313
#    elif __s390x__
#      define __NR_signalfd4 322
#    endif
#endif

#  ifndef __NR_signalfd
/* assume kernel headers are too old */
#    if __i386__
#      define __NR_signalfd 321
#    elif __x86_64__
#      define __NR_signalfd 282
#    elif __powerpc__
#      define __NR_signalfd 305
#    elif __s390x__
#      define __NR_signalfd 316
#    endif
#endif

int signalfd(int fd, const sigset_t *mask, int flags)
{
	int retval;

	retval = syscall (__NR_signalfd4, fd, mask, _NSIG / 8, flags);
	if (errno == ENOSYS && flags == 0)
		retval = syscall (__NR_signalfd, fd, mask, _NSIG / 8);
	return retval;
}
#endif

#if !HAVE_DECL_PR_CAPBSET_DROP
#define PR_CAPBSET_DROP 24
#endif

#include "error.h"
#include "af_unix.h"
#include "mainloop.h"

#include <lxc/lxc.h>
#include <lxc/log.h>

lxc_log_define(lxc_start, lxc);

LXC_TTY_HANDLER(SIGINT);
LXC_TTY_HANDLER(SIGQUIT);

static int setup_sigchld_fd(sigset_t *oldmask)
{
	sigset_t mask;
	int fd;

	if (sigprocmask(SIG_BLOCK, NULL, &mask)) {
		SYSERROR("failed to get mask signal");
		return -1;
	}

	if (sigaddset(&mask, SIGCHLD) || sigprocmask(SIG_BLOCK, &mask, oldmask)) {
		SYSERROR("failed to set mask signal");
		return -1;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		SYSERROR("failed to create the signal fd");
		return -1;
	}

	if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		SYSERROR("failed to set sigfd to close-on-exec");
		close(fd);
		return -1;
	}

	return fd;
}

static int setup_tty_service(const char *name, int *ttyfd)
{
	int fd;
	struct sockaddr_un addr = { 0 };
	char *offset = &addr.sun_path[1];
	
	strcpy(offset, name);
	addr.sun_path[0] = '\0';

	fd = lxc_af_unix_open(addr.sun_path, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		SYSERROR("failed to close-on-exec flag");
		close(fd);
		return -1;
	}

	*ttyfd = fd;

	return 0;
}

static int sigchld_handler(int fd, void *data, 
			   struct lxc_epoll_descr *descr)
{
	return 1;
}

static int ttyclient_handler(int fd, void *data,
			     struct lxc_epoll_descr *descr)
{
	int i;
 	struct lxc_tty_info *tty_info = data;

	for (i = 0; i < tty_info->nbtty; i++) {

		if (tty_info->pty_info[i].busy != fd)
			continue;

		lxc_mainloop_del_handler(descr, fd);
		tty_info->pty_info[i].busy = 0;
		close(fd);
	}

	return 0;
}

static int ttyservice_handler(int fd, void *data,
			      struct lxc_epoll_descr *descr)
{
	int conn, ttynum, val = 1, ret = -1;
	struct lxc_tty_info *tty_info = data;
	
	conn = accept(fd, NULL, 0);
	if (conn < 0) {
		SYSERROR("failed to accept tty client");
		return -1;
	}
	
	if (setsockopt(conn, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val))) {
		SYSERROR("failed to enable credential on socket");
		goto out_close;
	}

	if (lxc_af_unix_rcv_credential(conn, &ttynum, sizeof(ttynum)))
		goto out_close;

	if (ttynum > 0) {
		if (ttynum > tty_info->nbtty)
			goto out_close;

		if (tty_info->pty_info[ttynum - 1].busy)
			goto out_close;

		goto out_send;
	}

	/* fixup index tty1 => [0] */
	for (ttynum = 1;
	     ttynum <= tty_info->nbtty && tty_info->pty_info[ttynum - 1].busy;
	     ttynum++);

	/* we didn't find any available slot for tty */
	if (ttynum > tty_info->nbtty)
		goto out_close;

out_send:
	if (lxc_af_unix_send_fd(conn, tty_info->pty_info[ttynum - 1].master,
				&ttynum, sizeof(ttynum)) < 0) {
		ERROR("failed to send tty to client");
		goto out_close;
	}

	if (lxc_mainloop_add_handler(descr, conn,
				     ttyclient_handler, tty_info)) {
		ERROR("failed to add tty client handler");
		goto out_close;
	}

	tty_info->pty_info[ttynum - 1].busy = conn;

	ret = 0;
out:
	return ret;
out_close:
	close(conn);
	goto out;
}

int lxc_poll(const char *name, struct lxc_handler *handler)
{
	int sigfd = handler->sigfd;
	int pid = handler->pid;
	const struct lxc_tty_info *tty_info = &handler->tty_info;

	int nfds, ttyfd = -1, ret = -1;
	struct lxc_epoll_descr descr;

	if (tty_info->nbtty && setup_tty_service(name, &ttyfd)) {
		ERROR("failed to create the tty service point");
		goto out_sigfd;
	}

	/* sigfd + nb tty + tty service 
	 * if tty is enabled */
	nfds = tty_info->nbtty + 1 + tty_info->nbtty ? 1 : 0;

	if (lxc_mainloop_open(nfds, &descr)) {
		ERROR("failed to create mainloop");
		goto out_ttyfd;
	}

	if (lxc_mainloop_add_handler(&descr, sigfd, sigchld_handler, &pid)) {
		ERROR("failed to add handler for the signal");
		goto out_mainloop_open;
	}

	if (tty_info->nbtty) {
		if (lxc_mainloop_add_handler(&descr, ttyfd, 
					     ttyservice_handler, 
					     (void *)tty_info)) {
			ERROR("failed to add handler for the tty");
			goto out_mainloop_open;
		}
	}

	ret = lxc_mainloop(&descr);

out:
	return ret;

out_mainloop_open:
	lxc_mainloop_close(&descr);
out_ttyfd:
	close(ttyfd);
out_sigfd:
	close(sigfd);
	goto out;
}

static int save_init_pid(const char *name, pid_t pid)
{
	char init[MAXPATHLEN];
	char *val;
	int fd, err = -1;

	snprintf(init, MAXPATHLEN, LXCPATH "/%s/init", name);

	if (!asprintf(&val, "%d\n", pid)) {
		SYSERROR("failed to allocate memory");
		goto out;
	}

	fd = open(init, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		SYSERROR("failed to open '%s'", init);
		goto out_free;
	}

	if (write(fd, val, strlen(val)) < 0) {
		SYSERROR("failed to write the init pid");
		goto out_close;
	}

	err = 0;

out_close:
	close(fd);
out_free:
	free(val);
out:
	return err;
}

static void remove_init_pid(const char *name, pid_t pid)
{
	char init[MAXPATHLEN];

	snprintf(init, MAXPATHLEN, LXCPATH "/%s/init", name);
	unlink(init);
}

int lxc_init(const char *name, struct lxc_handler *handler)
{
	int err = -1;

	memset(handler, 0, sizeof(*handler));

	handler->lock = lxc_get_lock(name);
	if (handler->lock < 0)
		goto out;

	/* Begin the set the state to STARTING*/
	if (lxc_setstate(name, STARTING)) {
		ERROR("failed to set state '%s'", lxc_state2str(STARTING));
		goto out_put_lock;
	}

	/* If we are not attached to a tty, disable it */
	if (ttyname_r(0, handler->tty, sizeof(handler->tty)))
		handler->tty[0] = '\0';

	if (lxc_create_tty(name, &handler->tty_info)) {
		ERROR("failed to create the ttys");
		goto out_aborting;
	}

	/* the signal fd has to be created before forking otherwise
	 * if the child process exits before we setup the signal fd,
	 * the event will be lost and the command will be stuck */
	handler->sigfd = setup_sigchld_fd(&handler->oldmask);
	if (handler->sigfd < 0) {
		ERROR("failed to set sigchild fd handler");
		goto out_delete_tty;
	}

	/* Avoid signals from terminal */
	LXC_TTY_ADD_HANDLER(SIGINT);
	LXC_TTY_ADD_HANDLER(SIGQUIT);

	err = 0;
out:
	return err;

out_delete_tty:
	lxc_delete_tty(&handler->tty_info);
out_aborting:
	lxc_setstate(name, ABORTING);
out_put_lock:
	lxc_put_lock(handler->lock);
	goto out;
}

void lxc_fini(const char *name, struct lxc_handler *handler)
{
	/* The STOPPING state is there for future cleanup code
	 * which can take awhile
	 */
	lxc_setstate(name, STOPPING);

	lxc_setstate(name, STOPPED);

	remove_init_pid(name, handler->pid);

	lxc_delete_tty(&handler->tty_info);

	lxc_unlink_nsgroup(name);

	lxc_put_lock(handler->lock);

	LXC_TTY_DEL_HANDLER(SIGQUIT);
	LXC_TTY_DEL_HANDLER(SIGINT);
}

void lxc_abort(const char *name, struct lxc_handler *handler)
{
	lxc_setstate(name, ABORTING);
	kill(handler->pid, SIGKILL);
}

int lxc_spawn(const char *name, struct lxc_handler *handler, char *const argv[])
{
	int sv[2];
	int clone_flags;
	int err = -1, sync;

	/* Synchro socketpair */
	if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv)) {
		SYSERROR("failed to create communication socketpair");
		goto out;
	}

	clone_flags = CLONE_NEWPID|CLONE_NEWIPC|CLONE_NEWNS;
	if (conf_has_utsname(name))
		clone_flags |= CLONE_NEWUTS;
	if (conf_has_network(name))
		clone_flags |= CLONE_NEWNET;

	/* Create a process in a new set of namespaces */
	handler->pid = fork_ns(clone_flags);
	if (handler->pid < 0) {
		SYSERROR("failed to fork into a new namespace");
		goto out_close;
	}

	if (!handler->pid) {

		if (sigprocmask(SIG_SETMASK, &handler->oldmask, NULL)) {
			SYSERROR("failed to set sigprocmask");
			return -1;
		}

		close(sv[1]);

		/* Be sure we don't inherit this after the exec */
		fcntl(sv[0], F_SETFD, FD_CLOEXEC);
		
		/* Tell our father he can begin to configure the container */
		if (write(sv[0], &sync, sizeof(sync)) < 0) {
			SYSERROR("failed to write socket");
			goto out_child;
		}

		/* Wait for the father to finish the configuration */
		if (read(sv[0], &sync, sizeof(sync)) < 0) {
			SYSERROR("failed to read socket");
			goto out_child;
		}

		/* Setup the container, ip, names, utsname, ... */
		err = lxc_setup(name, handler->tty, &handler->tty_info);
		if (err) {
			ERROR("failed to setup the container");
			if (write(sv[0], &err, sizeof(err)) < 0)
				SYSERROR("failed to write the socket");
			goto out_child;
		}

		if (prctl(PR_CAPBSET_DROP, CAP_SYS_BOOT, 0, 0, 0)) {
			SYSERROR("failed to remove CAP_SYS_BOOT capability");
			goto out_child;
		}

		execvp(argv[0], argv);
		SYSERROR("failed to exec %s", argv[0]);

		err = LXC_ERROR_WRONG_COMMAND;
		/* If the exec fails, tell that to our father */
		if (write(sv[0], &err, sizeof(err)) < 0)
			SYSERROR("failed to write the socket");
		
	out_child:
		exit(err);
	}

	close(sv[0]);
	
	/* Wait for the child to be ready */
	if (read(sv[1], &sync, sizeof(sync)) < 0) {
		SYSERROR("failed to read the socket");
		goto out_abort;
	}

	if (lxc_link_nsgroup(name, handler->pid))
		WARN("cgroupfs not found: cgroup disabled");

	/* Create the network configuration */
	if (clone_flags & CLONE_NEWNET && conf_create_network(name, handler->pid)) {
		ERROR("failed to create the configured network");
		goto out_abort;
	}

	/* Tell the child to continue its initialization */
	if (write(sv[1], &sync, sizeof(sync)) < 0) {
		SYSERROR("failed to write the socket");
		goto out_abort;
	}

	/* Wait for the child to exec or returning an error */
	err = read(sv[1], &sync, sizeof(sync));
	if (err < 0) {
		ERROR("failed to read the socket");
		goto out_abort;
	}

	if (save_init_pid(name, handler->pid)) {
		ERROR("failed to save the init pid info");
		goto out_abort;
	}

	if (lxc_setstate(name, RUNNING)) {
		ERROR("failed to set state to %s",
			      lxc_state2str(RUNNING));
		goto out_abort;
	}

	err = 0;

out_close:
	close(sv[0]);
	close(sv[1]);
out:
	return err;

out_abort:
	lxc_abort(name, handler);
	goto out_close;
}

int lxc_start(const char *name, char *const argv[])
{
	struct lxc_handler handler = { 0 };
	int err = -LXC_ERROR_INTERNAL;
	int status;

	if (lxc_init(name, &handler)) {
		ERROR("failed to initialize the container");
		goto out;
	}

	err = lxc_spawn(name, &handler, argv);
	if (err) {
		ERROR("failed to spawn '%s'", argv[0]);
		goto out;
	}

	if (lxc_poll(name, &handler)) {
		ERROR("mainloop exited with an error");
		goto out_abort;
	}

	while (waitpid(handler.pid, &status, 0) < 0 && errno == EINTR)
		continue;
	err = 0;
out:
	lxc_fini(name, &handler);
	return err;

out_abort:
	lxc_abort(name, &handler);
	goto out;
}
