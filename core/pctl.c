/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <network_ipc.h>
#include <pctl.h>
#include <util.h>
#include <signal.h>
#include <sys/wait.h>
#include <parselib.h>
#include <swupdate_settings.h>

#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif

/* the array contains the pid of the subprocesses */
#define MAX_PROCESSES	10
static struct swupdate_task procs[MAX_PROCESSES];
static int    nprocs = 0;

/*
 * The global pid is used to identify if context is
 * the main process (SWUpdate, pid=0) or it is
 * a child process.
 * This is required when using internal libraries and can be
 * decided between a direct call or (in case of child process)
 * a IPC is required.
 */
int pid = 0;

/*
 * The sw_sockfd is used for internal ipc
 * with SWUpdate processes
 */

int sw_sockfd = -1;

#if defined(__linux__)
static void parent_dead_handler(int __attribute__ ((__unused__)) dummy)
{
	exit(1);
}
#endif

/*
 * This is used to spawn internal threads
 */
pthread_t start_thread(void *(* start_routine) (void *), void *arg)
{
	int ret;
	pthread_t id;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&id, &attr, start_routine, arg);
	if (ret) {
		exit(1);
	}
	return id;
}

/*
 * spawn_process forks and start a new process
 * under a new user
 */
static int spawn_process(struct swupdate_task *task,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgname,
			int ac, char **av,
			swupdate_process start,
			const char *cmdline)
{
	int process_id;
	int sockfd[2];

	/*
	 * Create the pipe to exchange data with the child
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0) {
		ERROR("socketpair fails : %s", strerror(errno));
		return -1;
	}

	process_id = fork();


	/*
	 * We need just one bidirectional pipe
	 */

	/*
	 * if father, it is finished
	 */
	if (process_id) {
		/* Parent close the [1] */
		close(sockfd[1]);
		task->pid = process_id;
		task->pipe = sockfd[0];
		return 0;
	}

	/* Child closes [0] */
	close(sockfd[0]);

	sw_sockfd = sockfd[1];

	/* process is running as root, drop privileges */
	if (getuid() == 0) {
		if (setgid(run_as_groupid) != 0) {
			ERROR("setgid: Unable to drop group privileges: %s", strerror(errno));
			return -1;
		}

		if (setuid(run_as_userid) != 0) {
			ERROR("setuid: Unable to drop user privileges: %s", strerror(errno));
			return -1;
		}
	}

	/* Save new pid */
	pid = getpid();

	notify_init();

#if defined(__linux__)
	if (signal(SIGUSR1, parent_dead_handler) == SIG_ERR) {
		/*
		 * this is not a reason to break, just a warning
		 */
		WARN("Cannot track if parent dies, sorry...");
	}

	if (prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0)
		ERROR("Error calling prctl");
#else
	WARN("Cannot track if parent dies on non-Linux OSes, sorry...");
#endif

	if (start)
		return (*start)(cfgname, ac, av);
	else {
		if (execvp(cmdline, av) == -1) {
			INFO("Spawning process %s failed: %s", av[0], strerror(errno));
			return -1;
		}
		return 0;
	}
}

static void start_swupdate_subprocess(sourcetype type,
			const char *name, const char *cfgfile,
			int argc, char **argv,
			swupdate_process start,
			const char *cmdline)
{
	uid_t uid;
	gid_t gid;

	read_settings_user_id(cfgfile, name, &uid, &gid);
	procs[nprocs].name = name;
	procs[nprocs].type = type;
	if (spawn_process(&procs[nprocs], uid, gid, cfgfile, argc, argv, start, cmdline) < 0) {
		ERROR("Spawning %s failed, exiting process...", name);
		exit(1);
	}

	TRACE("Started %s with pid %d and fd %d", name, procs[nprocs].pid, procs[nprocs].pipe);
	nprocs++;
}


void start_subprocess_from_file(sourcetype type, const char *name,
			const char *cfgfile,
			int argc, char **argv,
			const char *cmdline)
{
	start_swupdate_subprocess(type, name, cfgfile, argc, argv, NULL, cmdline);
}

void start_subprocess(sourcetype type, const char *name, const char *cfgfile,
			int argc, char **argv,
			swupdate_process start)
{

	start_swupdate_subprocess(type, name, cfgfile, argc, argv, start, NULL);
}

/*
 * run_system_cmd executes a shell script in background and intercepts
 * stdout and stderr of the script, writing then to TRACE and ERROR
 * This let the output of the scripts to be collected by SWUpdate
 * tracing capabilities.
 */
int run_system_cmd(const char *cmd)
{
	int ret = 0;
	int stdoutpipe[2];
	int stderrpipe[2];
	pid_t process_id;
	int const PIPE_READ = 0;
	int const PIPE_WRITE = 1;
	int wstatus;

	if (!strnlen(cmd, SWUPDATE_GENERAL_STRING_SIZE))
		return 0;

	if (strnlen(cmd, SWUPDATE_GENERAL_STRING_SIZE) > SWUPDATE_GENERAL_STRING_SIZE) {
		ERROR("Command string too long, skipping..");
		/* do nothing */
		return -EINVAL;
	}

	/*
	 * Creates pipes to intercept stdout and stderr of the
	 * child process
	 */
	if (pipe(stdoutpipe) < 0) {
		ERROR("stdout pipe cannot be created, exiting...");
		return -EFAULT;
	}
	if (pipe(stderrpipe) < 0) {
		ERROR("stderr pipe cannot be created, exiting...");
		close(stdoutpipe[0]);
		close(stdoutpipe[1]);
		return -EFAULT;
	}

	process_id = fork();
	/* Child process, this runs the shell command */
	if (process_id == 0) {
		if (dup2(stdoutpipe[PIPE_WRITE], STDOUT_FILENO) < 0)
			exit(errno);
		if (dup2(stderrpipe[PIPE_WRITE], STDERR_FILENO) < 0)
			exit(errno);
		/* close all pipes, not used anymore */
		close(stdoutpipe[PIPE_READ]);
		close(stdoutpipe[PIPE_WRITE]);
		close(stderrpipe[PIPE_READ]);
		close(stderrpipe[PIPE_WRITE]);

		ret = execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		if (ret) {
			ERROR("Process %s cannot be started: %s", cmd, strerror(errno));
			exit(1);
		}
	} else {
		int fds[2];
		pid_t w;

		close(stdoutpipe[PIPE_WRITE]);
		close(stderrpipe[PIPE_WRITE]);

		fds[0] = stdoutpipe[PIPE_READ];
		fds[1] = stderrpipe[PIPE_READ];

		/*
		 * Now waits until the child process exits and checks
		 * for the output. Forward data from stdout as TRACE
		 * and from stderr (of the child process) as ERROR
		 */
		do {
			int n1 = 0;
			struct timeval tv;
			fd_set readfds;
			int n, i;
			char buf[2][SWUPDATE_GENERAL_STRING_SIZE];
			char *pbuf;
			int cindex[2] = {0, 0}; /* this is the first free char inside buffer */

			w = waitpid(process_id, &wstatus, WNOHANG | WUNTRACED | WCONTINUED);
			if (w == -1) {
				ERROR("Error from waitpid() !!");
				close(stdoutpipe[PIPE_READ]);
				close(stderrpipe[PIPE_READ]);
				return -EFAULT;
			}

			/*
			 * Use buffers (for stdout and stdin) to collect data from
			 * the cmd. Data can contain multiple lines or just a part
			 * of a line and must be parsed
			 */
			memset(buf[0], 0, SWUPDATE_GENERAL_STRING_SIZE);
			memset(buf[1], 0, SWUPDATE_GENERAL_STRING_SIZE);

			tv.tv_sec = 1;
			tv.tv_usec = 0;

			/* Check if the child has sent something */
			do {
				FD_ZERO(&readfds);
				FD_SET(fds[0], &readfds);
				FD_SET(fds[1], &readfds);

				n1 = 0;
				ret = select(max(fds[0], fds[1]) + 1, &readfds, NULL, NULL, &tv);
				if (ret <= 0)
					break;

				for (i = 0; i < 2 ; i++) {
					pbuf = buf[i];
					int c = cindex[i];

					if (FD_ISSET(fds[i], &readfds)) {
						int last;

						n = read(fds[i], &pbuf[c], SWUPDATE_GENERAL_STRING_SIZE - c);
						if (n < 0)
							continue;
						n += c;	/* add previous data, if any */
						n1 += n;
						if (n > 0) {

							/* check if just a part of a line was sent. In that
							 * case, search for the last line and then copy the rest
							 * to the begin of the buffer for next read
							 */
							last = n - 1;
							while (last > 0 && pbuf[last] != '\0' && pbuf[last] != '\n')
									last--;
							pbuf[last] = '\0';
							/*
							 * compute the truncate line that should be
							 * parsed next time when the rest is received
							 */
							int left = n - 1 - last;
							char **lines = string_split(pbuf, '\n');
							int nlines = count_string_array((const char **)lines);

							if (last < SWUPDATE_GENERAL_STRING_SIZE - 1) {
								last++;
								memcpy(pbuf, &pbuf[last], left);
								if (last + left < SWUPDATE_GENERAL_STRING_SIZE) {
									memset(&pbuf[left], 0, SWUPDATE_GENERAL_STRING_SIZE - left);
								}
								cindex[i] = left;
							} else { /* no need to copy, reuse all buffer */
								memset(pbuf, 0, SWUPDATE_GENERAL_STRING_SIZE);
								cindex[i] = 0;
							}

							for (unsigned int index = 0; index < nlines; index++) {
								switch (i) {
								case 0:
									TRACE("%s", lines[index]);
									break;
								case 1:
									ERROR("%s", lines[index]);
									break;
								}
							}

							free_string_array(lines);
						}
					}
				}
			} while (ret > 0 && n1 > 0);
		} while (w != process_id);

		close(stdoutpipe[PIPE_READ]);
		close(stderrpipe[PIPE_READ]);

		if (WIFEXITED(wstatus)) {
			ret = WEXITSTATUS(wstatus);
			TRACE("%s command returned %d", cmd, ret);
		} else {
			TRACE("(%s) killed by signal %d\n", cmd, WTERMSIG(wstatus));
			ret = -1;
		}
	}

	return ret;
}

/*
 * The handler supervises the subprocesses
 * (Downloader, Webserver, Suricatta)
 * if one of them dies, SWUpdate exits
 * and sends a SIGTERM signal to all other subprocesses
 */
void sigchld_handler (int __attribute__ ((__unused__)) signum)
{
	int childpid, status, serrno;
	int exitstatus;
	int hasdied = 0;
	int i;

	serrno = errno;

	/*
	 * One process stops, find who is
	 */
	for (i = 0; i < nprocs; i++) {
		childpid = waitpid (procs[i].pid, &status, WNOHANG);
		if (childpid < 0) {
			perror ("waitpid, no child");
			continue;
		}
		if (childpid == 0)
			continue;

		if (procs[i].pid == childpid) {
			printf("Child %d(%s) ", childpid, procs[i].name);
			hasdied = 0;
			if (WIFEXITED(status)) {
				hasdied = 1;
				exitstatus = WEXITSTATUS(status);
				printf("exited, status=%d\n", exitstatus);
			} else if (WIFSIGNALED(status)) {
				hasdied = 1;
				exitstatus = WTERMSIG(status);
				printf("killed by signal %d\n", WTERMSIG(status));
			} else if (WIFSTOPPED(status)) {
				printf("stopped by signal %d\n", WSTOPSIG(status));
			} else if (WIFCONTINUED(status)) {
				printf("continued\n");
			}
			break;
		}
	}

	/*
	 * Communicate to all other processes that something happened
	 * and exit
	 */
	if (hasdied) {
		signal(SIGCHLD, SIG_IGN);
		for (i = 0; i < nprocs; i++) {
			if (procs[i].pid != childpid) {
				kill(procs[i].pid, SIGTERM);
			}
		}

		exit(exitstatus);
	}

	errno = serrno;
}

int pctl_getfd_from_type(sourcetype s)
{
	int i;

	for (i = 0; i < nprocs; i++) {
		if (s == procs[i].type)
			return procs[i].pipe;
	}

	return -ENODEV;
}

const char *pctl_getname_from_type(sourcetype s)
{
	int i;

	for (i = 0; i < nprocs; i++) {
		if (s == procs[i].type)
			return procs[i].name;
	}

	return "";
}
