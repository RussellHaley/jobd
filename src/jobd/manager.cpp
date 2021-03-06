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

#include <regex>
#include <unordered_set>
#include <iostream>
#include <fstream>

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

#include "clock.h"
#include "calendar.h"
#include "ipc.h"
#include <libjob/logger.h>
#include <libjob/jobProperty.hpp>
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

static void *keepalive_wake_handler = NULL; //kludge

static void setup_logging();
void run_pending_jobs(void);

launchd_options_t options;

void JobManager::setup(struct pidfh *pfh)
{
	this->pidfile_handle = pfh;

	if (getenv("JOBD_DEBUG_NOFORK")) {
		this->setNoFork(true);
	}

	if ((this->kqfd = kqueue()) < 0)
		err(1, "kqueue(2)");
	setup_logging();
	this->setupSignalHandlers();
	setup_socket_activation(this->kqfd);
	this->setupDataDirectory();
	if (setup_timers(this->kqfd) < 0)
		errx(1, "setup_timers()");
//FIXME	if (calendar_init(this->kqfd) < 0)
		// errx(1, "calendar_init()");
	if (ipc_init(this->kqfd) < 0)
		errx(1, "ipc_init()");

	this->scanJobDirectory();
}

void JobManager::defineJob(const string& path)
{
	unique_ptr<Job> job(new Job);

	log_debug("parsing %s", path.c_str());
	job->setManager(this);
	job->parseManifest(path);
	std::string label = job->getLabel();
	if (jobs.find(label) != jobs.end()) {
		log_error("Duplicate label detected");
		throw std::invalid_argument("Tried to add a job with a duplicate label");
	}

	job->jobStatus.setLabel(label);
	job->jobProperty.setLabel(label);

	// Write the parsed, normalized JSON back out to a file
	std::ofstream ofile;
	ofile.open(jobd_config.getManifestDir() + '/' + label + ".json");
	ofile << job->manifest.json.dump(4) << std::endl;
	ofile.close();

	if (!jobs.insert(std::make_pair(label, std::move(job))).second) {
		// should not happen because we check earlier, but..
		log_error("Duplicate label detected");
		throw std::invalid_argument("Tried to add a job with a duplicate label");
	}
}

void JobManager::scanJobDirectory()
{
	DIR	*dirp;
	struct dirent entry, *result;
	size_t job_count = 0;
	size_t pending_jobs = 0;
	std::string jobdir = jobd_config.getManifestDir();

	log_debug("scanning %s for jobs", jobdir.c_str());
	if ((dirp = opendir(jobdir.c_str())) == NULL)
		err(1, "opendir(3)");

	while (dirp) {
		if (readdir_r(dirp, &entry, &result) < 0)
			err(1, "readdir_r(3)");
		if (!result) break;
		if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) {
			continue;
		}

		std::string d_name = std::string(entry.d_name);
		std::string path = jobdir + "/" + d_name;
		try {
			defineJob(path);
		} catch (std::system_error& e) {
			log_error("error parsing %s: %s", path.c_str(), e.what());
		}
	}
	if (closedir(dirp) < 0)
		err(1, "closedir(3)");

	log_debug("finished scanning jobs: total=%zu new=%zu", job_count, pending_jobs);

	runPendingJobs();
}

void JobManager::runPendingJobs()
{
	/* Pass #1: load all jobs */
	for (auto& it : this->jobs) {
		const string &label = it.first;
		unique_ptr<Job>& job = it.second;

		if (job->getState() == JOB_STATE_DEFINED) {
			log_debug("loading job: %s", label.c_str());
			job->load();
		}

		bool auto_enable = job->manifest.json["Enable"];
		if (auto_enable) {
			job->jobProperty.setEnabled(true);
		}
	}

	/* Pass #2: run all loaded jobs that are runnable */
	for (auto& it : this->jobs) {
		const string &label = it.first;
		unique_ptr<Job>& job = it.second;

		if (job->getState() == JOB_STATE_LOADED && job->isRunnable()) {
			log_debug("running job: %s", label.c_str());
			job->run();
		} else {
			log_debug("not running job %s; state=%s is_runnable=%d",
					label.c_str(), job->getStateString().c_str(), job->isRunnable());

		}
	}
}

void JobManager::wakeJob(const string& label)
{
	unique_ptr<Job>& job = this->jobs.find(label)->second;

	if (job->state != JOB_STATE_WAITING) {
		log_error("tried to wake job %s that was not asleep (state=%d)",
				job->getLabel().c_str(), job->state);
		throw std::logic_error("Tried to wake a job that was not asleep");
	}
	job->run();
}

void JobManager::removeJob(Job& job) {
	this->jobs.erase(job.getLabel());
}

void JobManager::clearJob(const string& label) {
	unique_ptr<Job>& job = this->jobs.find(label)->second;
	job->clearFault();
}

void JobManager::enableJob(const string& label) {
	unique_ptr<Job>& job = this->jobs.find(label)->second;
	if (job->isEnabled()) {
		log_warning("tried to enable a job that was already enabled");
	} else {
		job->setEnabled(true);
		log_debug("job %s enabled", label.c_str());
	}
}

void JobManager::disableJob(const string& label) {
	unique_ptr<Job>& job = this->getJobByLabel(label);

	if (job->isEnabled()) {
		job->setEnabled(false);
		log_debug("job %s disabled", label.c_str());
	} else {
		log_warning("tried to disable a job that was already disabled");
	}
}

void JobManager::unloadJob(unique_ptr<Job>& job) {
	string label = job->getLabel();

	job->unload();
	log_debug("job %s unloaded", label.c_str());

	if (job->getState() == JOB_STATE_DEFINED) {
		deleteJob(job);
	} else {
		log_debug("job deletion deferred; state=%s", job->getStateString().c_str());
	}
}

void JobManager::unloadJob(const string& label) {
	auto it = jobs.find(label);
	if (it == jobs.end())
		throw std::invalid_argument("label not found");

	unique_ptr<Job>& job = it->second;
	this->unloadJob(job);
}

void JobManager::unloadAllJobs()
{
	for (auto& iter : this->jobs) {
		iter.second->unload();
		//FIXME: this->jobs.erase(iter.second->getLabel());
	}
}

void JobManager::createProcessEventWatch(pid_t pid)
{
	struct kevent kev;

	EV_SET(&kev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
	if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		log_errno("kevent");
		//TODO: probably want to crash, or kill the job, or do something
		// more useful here.
	}
	log_debug("will be notified if process %d exits", pid);
}

void JobManager::deleteProcessEventWatch(pid_t pid)
{
	struct kevent kev;

	/* This isn't necessary, I think, but just to be on the safe side.. */
	EV_SET(&kev, pid, EVFILT_PROC, EV_DELETE, NOTE_EXIT, 0, NULL);
	if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		if (errno != ENOENT)
			err(1, "kevent");
	}
}

//DEADWOOD
#if 0
void JobManager::monitorJobDirectory()
{
	struct kevent kev;
	int fd;

	fd = open(jobd_config.getManifestDir().c_str(), O_RDONLY);
	if (fd < 0) {
		log_errno("open(2)");
		throw std::system_error(errno, std::system_category());
	}

	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);

	EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
			NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB, 0, 0);
	if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		log_errno("kevent(2)");
		throw std::system_error(errno, std::system_category());
	}
}
#endif

unique_ptr<Job>& JobManager::getJobByLabel(const string& label)
{
	auto it = this->jobs.find(label);
	if (it == this->jobs.end())
	{
		throw std::out_of_range("job not found with label: %s" + label);
	}

	unique_ptr<Job>& job = it->second;
	return job;
}

unique_ptr<Job>& JobManager::getJobByPid(pid_t pid)
{
	for (auto& it : this->jobs) {
		unique_ptr<Job>& job = it.second;
		if (job->jobStatus.getPid() == pid) {
			return job;
		}
	}
	throw std::out_of_range("job not found");
}

void JobManager::reapChildProcess(pid_t pid, int status)
{
	int status2;

	if (waitpid(pid, &status2, WNOHANG) != pid) {
		log_errno("waitpid(2)");
	}

	deleteProcessEventWatch(pid);

	try {
		unique_ptr<Job>& job = this->getJobByPid(pid);

		int last_exit_status, term_signal;
		if (WIFEXITED(status)) {
			last_exit_status = WEXITSTATUS(status);
			term_signal = 0;
		} else if (WIFSIGNALED(status)) {
			last_exit_status = -1;
			term_signal = WTERMSIG(status);
		} else {
			term_signal = -1;
			last_exit_status = -1;
			log_error("unhandled exit status");
		}
		log_debug("job %d exited with status=%d term_signal=%d",
				job->jobStatus.getPid(), last_exit_status, term_signal);

		//TODO: these three calls cause sync() to run three times.
		// would like one function that sets all three,
		// such as a Job::reap(last_exit_status, term_signal) function
		job->jobStatus.setLastExitStatus(last_exit_status);
		job->jobStatus.setTermSignal(term_signal);
		job->jobStatus.setPid(0);

		this->rescheduleJob(job);
	} catch (std::out_of_range& e) {
		log_warning("child pid %d exited but no job found", pid);
		return;
	}
}

void JobManager::deleteJob(unique_ptr<Job>& job)
{
	string manifest_path = jobd_config.getManifestDir() + '/' + job->getLabel() + ".json";

	if (unlink(manifest_path.c_str()) < 0) {
		log_error("unlink(2) of %s", manifest_path.c_str());
	}

	job->releaseAllResources();

	jobs.erase(job->getLabel());
	//XXX-will probably leak memory here, need to ::delete job
}

void JobManager::rescheduleJob(unique_ptr<Job>& job) {
	if (!job->isLoaded()) {
		log_debug("deleting job");
		deleteJob(job);
		return;
	}

	if (job->state == JOB_STATE_KILLED && !job->isEnabled()) {
		log_debug("job `%s' is disabled and will not be rescheduled", job->getLabel().c_str());
		job->state = JOB_STATE_LOADED;
		return;
	}

	if (job->manifest.json["StartInterval"].get<unsigned long>() > 0) {
		job->state = JOB_STATE_WAITING;
	} else {
		job->state = JOB_STATE_EXITED;
	}

	if (job->manifest.json["KeepAlive"].get<bool>()) {
		unsigned int interval = job->manifest.json["ThrottleInterval"].get<unsigned int>();

		log_debug("will restart job %s after %u seconds",
				job->getLabel().c_str(), interval);

		job->restart_after = current_time() +
			job->manifest.json["ThrottleInterval"].get<unsigned int>();
	} else {
		log_debug("marking job as faulted");
		// Assume that non-KeepAlive jobs are supposed to run forever
		// FIXME: For on-demand jobs, this should not be a fault.
		job->jobProperty.setFaulted(libjob::JobProperty::JOB_FAULT_STATE_OFFLINE,
				"The process exited unexpectedly");
	}

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

void JobManager::setupDataDirectory()
{
	std::string manifestdir = jobd_config.getManifestDir();

	log_debug("creating %s", manifestdir.c_str());
	mkdir_idempotent(manifestdir.c_str(), 0700);

	std::string jobStatusRuntimeDir = jobd_config.getRuntimeDir() + std::string("/status");
	libjob::JobStatus::setRuntimeDirectory(jobStatusRuntimeDir);

	std::string jobPropertyDataDir = jobd_config.getDataDir() + std::string("/property");
	libjob::JobProperty::setDataDirectory(jobPropertyDataDir);
#if 0
	char buf[PATH_MAX];

	/* Clear any record of active jobs that may be leftover from a previous program crash */
        path_sprintf(&buf, "%s", options.activedir);
        delete_directory_entries(buf);

        path_sprintf(&buf, "%s", options.watchdir);
        delete_directory_entries(buf);
#endif
}

void JobManager::setupSignalHandlers()
{
	struct kevent kev;

	for (int i = 0; launchd_signals[i] != 0; i++) {
		if (signal(launchd_signals[i], SIG_IGN) == SIG_ERR)
			err(1, "signal(2): %d", launchd_signals[i]);
		EV_SET(&kev, launchd_signals[i], EVFILT_SIGNAL, EV_ADD, 0, 0,
				(void *)&launchd_signals);
		if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0)
			err(1, "kevent(2)");
	}
}

void JobManager::handleKeepaliveWakeup()
{
	time_t now = current_time();

	log_debug("watchdog handler running");

	/* Determine the soonest absolute time we should wake up */
	for (auto& it : this->jobs)
	{
		const string& label = it.first;
		unique_ptr<Job>& job = it.second;

		if (job->state == JOB_STATE_EXITED && job->restart_after <= now)
		{
			log_debug("job `%s' restarted via KeepAlive", label.c_str());
			job->run();
		}
	}
	this->updateKeepaliveWakeInterval();
}

void JobManager::updateKeepaliveWakeInterval()
{
	struct kevent kev;
	time_t new_wakeup_time = std::numeric_limits<time_t>::max();

	/* Determine the soonest absolute time we should wake up */
	for (auto& it : this->jobs)
	{
		unique_ptr<Job>& job = it.second;

		if (job->state == JOB_STATE_EXITED && job->restart_after > 0)
		{
			if (job->restart_after < new_wakeup_time) {
				new_wakeup_time = job->restart_after;
			}
		}

	}

	/* Stop waking up if there are no more jobs to be restarted. */
	if (new_wakeup_time == std::numeric_limits<time_t>::max() &&
			this->next_keepalive_wakeup > 0) {
		EV_SET(&kev, JOB_SCHEDULE_KEEPALIVE, EVFILT_TIMER, EV_ADD | EV_DISABLE, 0, 0, (void *)&keepalive_wake_handler);
		if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
			err(1, "kevent(2)");
		}
		log_debug("disabling keepalive wakeups");
	} else if (new_wakeup_time != this->next_keepalive_wakeup) {
		int time_delta = (new_wakeup_time - current_time()) * 1000;
		if (time_delta <= 0) {
			log_warning("the system clock appears to have gone backwards");
			time_delta = DEFAULT_THROTTLE_INTERVAL * 1000;
		}
		EV_SET(&kev, JOB_SCHEDULE_KEEPALIVE, EVFILT_TIMER,
			EV_ADD | EV_ENABLE, 0, time_delta, (void *)&keepalive_wake_handler);
		if (kevent(this->kqfd, &kev, 1, NULL, 0, NULL) < 0) {
			err(1, "kevent(2)");
		}

		log_debug("scheduled next wakeup event in %d ms at t=%ld",
				time_delta, (long)this->next_keepalive_wakeup);
	}
}

void JobManager::listAllJobs(nlohmann::json& result)
{
	result = nlohmann::json::object();

	for (auto& it : this->jobs) {
		const string &label = it.first;
		unique_ptr<Job>& job = it.second;

		result[label] = {
			{ "Pid", job->getPid() },
			{ "State", job->getStateString() },
			{ "Enabled", job->isEnabled() },
			{ "FaultState", job->getFaultStateString(), },
		};
	}
}

void JobManager::mainLoop()
{
	int rv;
	struct kevent kev;

	for (;;) {
		rv = kevent(this->kqfd, NULL, 0, &kev, 1, NULL);
		if (rv == 0) {
			log_debug("spurious wakeup; no events pending");
			continue;
		}
		if (rv < 0) {
			if (errno == EINTR) {
				log_warning("spurious wakeup; got signal"); // should not happen..
				continue;
			} else {
				err(1, "kevent(2)");
			}
		}
		/* TODO: refactor this to eliminate the use of switch() and just jump directly to the handler function */
		if ((void *)kev.udata == &launchd_signals) {
			switch (kev.ident) {
			case SIGHUP:
				this->scanJobDirectory();
				break;
			case SIGUSR1:
				//DEADWOOD: manager_write_status_file();
				break;
			case SIGCHLD:
				/* NOTE: undocumented use of kev.data to obtain
				 * the status of the child. This should be reported to
				 * FreeBSD as a bug in the manpage.
				 */
				this->reapChildProcess(kev.ident, kev.data);
				break;
			case SIGINT:
				log_notice("caught SIGINT, exiting");
				this->unloadAllJobs();
				exit(1);
				break;
			case SIGTERM:
				log_notice("caught SIGTERM, exiting");
				exit(0);
				break;
			default:
				log_error("caught unexpected signal");
			}
		} else if (kev.filter == EVFILT_PROC) {
			this->reapChildProcess(kev.ident, kev.data);
		} else if (kev.filter == EVFILT_VNODE) {
			this->scanJobDirectory();
		} else if ((void *)kev.udata == &setup_socket_activation) {
			if (socket_activation_handler() < 0)
				errx(1, "socket_activation_handler()");
		} else if ((void *)kev.udata == &setup_timers) {
			if (timer_handler() < 0)
				errx(1, "timer_handler()");
#if 0
			//FIXME
		} else if ((void *)kev.udata == &calendar_init) {
			if (calendar_handler() < 0)
				errx(1, "calendar_handler()");
#endif
		} else if ((void *)kev.udata == &keepalive_wake_handler) {
			this->handleKeepaliveWakeup();
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

void JobManager::forkHandler()
{
	ipc_fork_handler();
#ifndef UNIT_TEST
	closelog();
#endif
}

JobManager::~JobManager()
{
	if (this->pidfile_handle)
		pidfile_remove(pidfile_handle);
	ipc_shutdown();
}
