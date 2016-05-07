/*
 * Copyright (c) 2015 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "../../vendor/FreeBSD/sys/queue.h"

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/wait.h>
}

#include "../config.h"

#include "calendar.h"
#include "ipc.h"
#include <libjob/logger.h>
#include "job.h"
#include "keepalive.h"
#include "manager.h"
#include "pidfile.h"
#include "socket.h"
#include "options.h"
#include "timer.h"
#include "util.h"

#include "../libjob/job.h"
#include "../libjob/namespaceImport.hpp"

/* A list of signals that are meaningful to launchd(8) itself. */
const int launchd_signals[5] = {
	SIGHUP, SIGUSR1, SIGINT, SIGTERM, 0
};

static void setup_job_dirs();
static void setup_signal_handlers();
static void setup_logging();
void run_pending_jobs(void);
static void do_shutdown();

struct pidfh *pidfile_handle;
launchd_options_t options;

static map<string,unique_ptr<Job>> all_jobs;

static LIST_HEAD(,job) jobs;			/* All active jobs */

libjob::jobdConfig* jobd_config;

/* The kqueue descriptor used by main_loop() */
static int main_kqfd = -1;

void manager_init(struct pidfh *pfh)
{
	pidfile_handle = pfh;
	LIST_INIT(&jobs);

	try {
		 jobd_config = new libjob::jobdConfig();
	}
	catch (...) {
		errx(1, "libjob failed to init");
	}
	if ((main_kqfd = kqueue()) < 0)
		err(1, "kqueue(2)");
	setup_logging();
	setup_signal_handlers();
	setup_socket_activation(main_kqfd);
	setup_job_dirs();
	if (keepalive_init(main_kqfd) < 0)
		errx(1, "keepalive_init()");
	if (setup_timers(main_kqfd) < 0)
		errx(1, "setup_timers()");
	if (calendar_init(main_kqfd) < 0)
		errx(1, "calendar_init()");
	if (ipc_init(main_kqfd) < 0)
		errx(1, "ipc_init()");
}

static ssize_t poll_watchdir()
{
	DIR	*dirp;
	struct dirent entry, *result;
	size_t job_count = 0;
	size_t pending_jobs = 0;
	const char *jobdir = jobd_config->jobdir.c_str();

	log_debug("scanning %s for jobs", jobdir);
	if ((dirp = opendir(jobdir)) == NULL)
		err(1, "opendir(3)");

	while (dirp) {
		if (readdir_r(dirp, &entry, &result) < 0)
			err(1, "readdir_r(3)");
		if (!result) break;
		if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) {
			continue;
		}

		std::string path = jobd_config->jobdir + "/"
				+ std::string(entry.d_name);
		try {
			log_debug("parsing %s", path.c_str());
			unique_ptr<Job> job(new Job);
			job->parseManifest(path);
			if (!all_jobs.insert(std::make_pair(job->getLabel(), std::move(job))).second) {
				log_error("Duplicate label detected");
				continue;
			} else {
				pending_jobs++;
			}
		} catch (std::exception& e) {
			log_error("error parsing %s: %s", path.c_str(), e.what());
		}
		job_count++;
	}
	if (closedir(dirp) < 0)
		err(1, "closedir(3)");

	log_debug("finished scanning jobs: total=%zu new=%zu", job_count, pending_jobs);

	if (pending_jobs > 0) {
		run_pending_jobs();
	}

	return (0);
}

void run_pending_jobs(void)
{
	/* Pass #1: load all jobs */
	for (auto& it : all_jobs) {
		const string &label = it.first;
		unique_ptr<Job>& job = it.second;

		if (job->getState() == JOB_STATE_DEFINED) {
			log_debug("loading job: %s", label.c_str());
			job->load();
		}
	}

	/* Pass #2: run all loaded jobs that are runnable */
	for (auto& it : all_jobs) {
		const string &label = it.first;
		unique_ptr<Job>& job = it.second;

		if (job->getState() == JOB_STATE_LOADED && job->isRunnable()) {
			log_debug("running job: %s", label.c_str());
			job->run();
		}
	}

//fixme
#if 0
	job_manifest_t jm, jm_tmp;
	job_t job, job_tmp;
	LIST_HEAD(,job) joblist;

	LIST_INIT(&joblist);

	/* Pass #1: load all jobs */
	LIST_FOREACH_SAFE(jm, &pending, jm_le, jm_tmp) {
		job = job_new(jm);
		if (!job)
			errx(1, "job_new()");

		/* Check for duplicate jobs */
		if (manager_get_job_by_label(jm->label)) {
			log_error("tried to load a duplicate job with label %s", jm->label);
			job_free(job);
			continue;
		}

		LIST_INSERT_HEAD(&joblist, job, joblist_entry);
		(void) job_load(job); // FIXME failure handling?
		log_debug("loaded job: %s", job->jm->label);
	}
	LIST_INIT(&pending);

	/* Pass #2: run all loaded jobs */
	LIST_FOREACH(job, &joblist, joblist_entry) {
		if (job_is_runnable(job)) {
			log_debug("running job %s from state %d", job->jm->label, job->state);
			(void) job_run(job); // FIXME failure handling?
		}
	}

	/* Pass #3: move all new jobs to the main jobs list */
	LIST_FOREACH_SAFE(job, &joblist, joblist_entry, job_tmp) {
		LIST_REMOVE(job, joblist_entry);
		LIST_INSERT_HEAD(&jobs, job, joblist_entry);
	}
#endif
}

int manager_wake_job(job_t job)
{
	if (job->state != JOB_STATE_WAITING) {
		log_error("tried to wake job %s that was not asleep (state=%d)",
				job->jm->label, job->state);
		return -1;
	}

	return job_run(job);
}

int manager_activate_job_by_fd(int fd)
{
	return -1; //STUB
}

job_t manager_get_job_by_label(const char *label)
{
	job_t job;

	LIST_FOREACH(job, &jobs, joblist_entry) {
		if (strcmp(label, job->jm->label) == 0) {
			return job;
		}
	}
	return NULL;
}

job_t manager_get_job_by_pid(pid_t pid)
{
	job_t job;

	LIST_FOREACH(job, &jobs, joblist_entry) {
		if (job->pid == pid) {
			return job;
		}
	}
	return NULL;
}

int manager_write_status_file()
{
	char *path, *buf, *pid;
	int fd;
	ssize_t len;
	job_t job;

	/* FIXME: should write to a .new file, then rename() over the old file */
	if (asprintf(&path, "%s/launchctl.list", jobd_config->jobdir.c_str()) < 0)
		err(1, "asprintf(3)");
	if ((fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0)
		err(1, "open(2)");
	if (asprintf(&buf, "%-8s %-8s %s\n", "PID", "Status", "Label") < 0)
		err(1, "asprintf(3)");
	len = strlen(buf) + 1;
	if (write(fd, buf, len) < len)
		err(1, "write(2)");
	free(buf);
	LIST_FOREACH(job, &jobs, joblist_entry) {
		if (job->pid == 0) {
			if ((pid = strdup("-")) == NULL)
				err(1, "strdup(3)");
		} else {
			if (asprintf(&pid, "%d", job->pid) < 0)
				err(1, "asprintf(3)");
		}
		if (asprintf(&buf, "%-8s %-8d %s\n", pid, job->last_exit_status, job->jm->label) < 0)
			err(1, "asprintf(3)");
		len = strlen(buf) + 1;
		if (write(fd, buf, len) < len)
			err(1, "write(2)");
		free(buf);
		free(pid);
	}
	if (close(fd) < 0)
		err(1, "close(2)");
	free(path);
	return 0;
}

void manager_free_job(job_t job) {
	// todo: remove from job database once this is a thing
#if 0
	char path[PATH_MAX];
	path_sprintf(&path, "%s/%s.json", options.activedir, job->jm->label);
	if (unlink(path) < 0) {
		log_errno("unlink(2) of %s", path);
	}
#endif
	LIST_REMOVE(job, joblist_entry);
	job_free(job);
}

int manager_unload_job(const char *label)
{
	job_t job, job_tmp, job_cur;
	int retval = -1;
	char *path = NULL;

	job = NULL;
	LIST_FOREACH_SAFE(job_cur, &jobs, joblist_entry, job_tmp) {
		if (strcmp(job_cur->jm->label, label) == 0) {
			job = job_cur;
			break;
		}
	}

	if (!job) {
		log_error("job not found: %s", label);
		goto out;
	}

	if (job_unload(job) < 0) {
		goto out;
	}

	log_debug("job %s unloaded", label);

	if (job->state == JOB_STATE_DEFINED) {
		manager_free_job(job);
	}

	retval = 0;

out:
	free(path);
	return retval;
}

void
manager_unload_all_jobs()
{
	job_t job;
	job_t job_tmp;

	log_debug("unloading all jobs");
	LIST_FOREACH_SAFE(job, &jobs, joblist_entry, job_tmp) {
		if (job_unload(job) < 0) {
			log_error("job unload failed: %s", job->jm->label);
		} else {
			log_debug("job %s unloaded", job->jm->label);
		}
		manager_free_job(job);
	}
}



void manager_update_jobs()
{
	if (poll_watchdir() > 0) {
		run_pending_jobs();
	}
}

void
manager_pid_event_add(int pid)
{
	struct kevent kev;

	EV_SET(&kev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
	if (kevent(main_kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		log_errno("kevent");
		//TODO: probably want to crash, or kill the job, or do something
		// more useful here.
	}
}

void
manager_pid_event_delete(int pid)
{
	struct kevent kev;

	/* This isn't necessary, I think, but just to be on the safe side.. */
	EV_SET(&kev, pid, EVFILT_PROC, EV_DELETE, NOTE_EXIT, 0, NULL);
	if (kevent(main_kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		if (errno != ENOENT)
			err(1, "kevent");
	}
}

void 
manager_reap_child(pid_t pid, int status)
{
	job_t job;

// linux will need to do this in a loop after reading a signalfd and getting SIGCHLD
#if 0
	pid = waitpid(-1, &status, WNOHANG);
	if (pid < 0) {
		if (errno == ECHILD) return;
		err(1, "waitpid(2)");
	} else if (pid == 0) {
		return;
	}
#endif

	manager_pid_event_delete(pid);

	job = manager_get_job_by_pid(pid);
	if (!job) {
		log_error("child pid %d exited but no job found", pid);
		return;
	}

	if (job->state == JOB_STATE_KILLED) {
		/* The job is unloaded, so nobody cares about the exit status */
		manager_free_job(job);
		return;
	}

	if (job->jm->start_interval > 0) {
		job->state = JOB_STATE_WAITING;
	} else {
		job->state = JOB_STATE_EXITED;
	}
	if (WIFEXITED(status)) {
		job->last_exit_status = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		job->last_exit_status = -1;
		job->term_signal = WTERMSIG(status);
	} else {
		log_error("unhandled exit status");
	}
	log_debug("job %d exited with status %d", job->pid,
			job->last_exit_status);
	job->pid = 0;

	if (keepalive_add_job(job) < 0)
		log_error("keepalive_add_job()");

	return;
}

#if 0
/* Delete everything in a given directory */
static void
delete_directory_entries(const char *path)
{
	DIR *dirp;
	struct dirent *ent;

	dirp = opendir(path);
	if (!dirp) err(1, "opendir(3) of %s", path);

	for (;;) {
		errno = 0;
		ent = readdir(dirp);
		if (!ent)
			break;

		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		log_debug("deleting manifest: %s/%s", path, ent->d_name);
		if (unlinkat(dirfd(dirp), ent->d_name, 0) < 0)
			err(1, "unlinkat(2)");
	}
	(void) closedir(dirp);
}
#endif

static void
setup_job_dirs()
{
	log_debug("creating %s", jobd_config->jobdir.c_str());
	mkdir_idempotent(jobd_config->jobdir.c_str(), 0700);
#if 0
	char buf[PATH_MAX];

	/* Clear any record of active jobs that may be leftover from a previous program crash */
        path_sprintf(&buf, "%s", options.activedir);
        delete_directory_entries(buf);

        path_sprintf(&buf, "%s", options.watchdir);
        delete_directory_entries(buf);
#endif
}

static void setup_signal_handlers()
{
	int i;
	struct kevent kev;

	for (i = 0; launchd_signals[i] != 0; i++) {
		if (signal(launchd_signals[i], SIG_IGN) == SIG_ERR)
			err(1, "signal(2): %d", launchd_signals[i]);
		EV_SET(&kev, launchd_signals[i], EVFILT_SIGNAL, EV_ADD, 0, 0,
				(void *)&setup_signal_handlers);
		if (kevent(main_kqfd, &kev, 1, NULL, 0, NULL) < 0)
			err(1, "kevent(2)");
	}
}

void
manager_main_loop()
{
	struct kevent kev;

	for (;;) {
		if (kevent(main_kqfd, NULL, 0, &kev, 1, NULL) < 1) {
			if (errno == EINTR) {
				continue;
			} else {
				err(1, "kevent(2)");
			}
		}
		/* TODO: refactor this to eliminate the use of switch() and just jump directly to the handler function */
		if ((void *)kev.udata == &setup_signal_handlers) {
			switch (kev.ident) {
			case SIGHUP:
				manager_update_jobs();
				break;
			case SIGUSR1:
				manager_write_status_file();
				break;
			case SIGCHLD:
				/* NOTE: undocumented use of kev.data to obtain
				 * the status of the child. This should be reported to
				 * FreeBSD as a bug in the manpage.
				 */
				manager_reap_child(kev.ident, kev.data);
				break;
			case SIGINT:
				log_notice("caught SIGINT, exiting");
				manager_unload_all_jobs();
				do_shutdown();
				exit(1);
				break;
			case SIGTERM:
				log_notice("caught SIGTERM, exiting");
				do_shutdown();
				exit(0);
				break;
			default:
				log_error("caught unexpected signal");
			}
		} else if (kev.filter == EVFILT_PROC) {
			(void) manager_reap_child(kev.ident, kev.data);
		} else if ((void *)kev.udata == &setup_socket_activation) {
			if (socket_activation_handler() < 0)
				errx(1, "socket_activation_handler()");
		} else if ((void *)kev.udata == &setup_timers) {
			if (timer_handler() < 0)
				errx(1, "timer_handler()");
		} else if ((void *)kev.udata == &calendar_init) {
			if (calendar_handler() < 0)
				errx(1, "calendar_handler()");
		} else if ((void *)kev.udata == &keepalive_wake_handler) {
			keepalive_wake_handler();
		} else if ((void *)kev.udata == &ipc_request_handler) {
			ipc_request_handler();
		} else {
			log_warning("spurious wakeup, no known handlers");
		}
	}
}

static void setup_logging()
{
#ifndef UNIT_TEST
	openlog("launchd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
#endif
}

static void do_shutdown()
{
	if (pidfile_handle)
		pidfile_remove(pidfile_handle);
	delete jobd_config;
	ipc_shutdown();
}

