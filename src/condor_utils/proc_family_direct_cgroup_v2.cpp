/***************************************************************
 *
 * Copyright (C) 1990-2022, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "condor_daemon_core.h"
#include "condor_uid.h"
#include "directory.h"
#include "proc_family_direct_cgroup_v2.h"
#include <numeric>

#include <filesystem>

namespace stdfs = std::filesystem;

static std::map<pid_t, std::string> cgroup_map;

static stdfs::path cgroup_mount_point() {
	return "/sys/fs/cgroup";
}

// mkdir the cgroup, and all required interior cgroups.  Note that the leaf
// cgroup in v2 cannot have anything in .../cgroup_subtree_control, or else
// we can't put a process in it.  Interior nodes *must* have the controllers
// put in that file, or else we won't have any controllers to query in
// our interior nodes.
bool 
ProcFamilyDirectCgroupV2::cgroupify_process(const std::string &cgroup_name, pid_t pid) {
	dprintf(D_FULLDEBUG, "Creating cgroup %s for pid %d\n", cgroup_name.c_str(), pid);

	TemporaryPrivSentry sentry( PRIV_ROOT );

	// Start from the root of the cgroup mount point
	stdfs::path cgroup_root_dir = cgroup_mount_point();

	stdfs::path cgroup_relative_to_root_dir(cgroup_name);

	// If the full cgroup exists, remove it to clear the various
	// peak statistics and any existing memory
	int r = rmdir((cgroup_root_dir / cgroup_name).c_str());
	if ((r < 0) && (errno != ENOENT))  {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::track_family_via_cgroup error removing cgroup %s: %s\n", cgroup_name.c_str(), strerror(errno));
	}

	// Accumulate along path components
	auto interior_dir_maker = [](const stdfs::path &fulldir, const stdfs::path &next_part) {
		stdfs::path interior = fulldir / next_part;
		mkdir_and_parents_if_needed(interior.c_str(), 0755, 0755, PRIV_ROOT);

		// Now that we've made our interior node
		// we need to tell it which cgroup controllers *it's* child has
		stdfs::path controller_filename = interior / "cgroup.subtree_control";
		int fd = open(controller_filename.c_str(), O_WRONLY, 0666);
		if (fd > 0) {
			// TODO: write these individually
			const char *child_controllers = "+cpu +io +memory +pids";
			int r = write(fd, child_controllers, strlen(child_controllers));
			if (r < 0) {
				dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::track_family_via_cgroup error writing to %s: %s\n", controller_filename.c_str(), strerror(errno));
			}
			close(fd);
		}
		return interior;
	};

	// cdr down the path, making all the interior nodes, skipping the last (leaf) one
	std::accumulate(cgroup_relative_to_root_dir.begin(), --cgroup_relative_to_root_dir.end(),
			cgroup_root_dir, interior_dir_maker);

	// Now the leaf cgroup
	stdfs::path leaf = cgroup_root_dir / cgroup_relative_to_root_dir;

	bool can_make_cgroup_dir = mkdir_and_parents_if_needed(leaf.c_str(), 0755, 0755, PRIV_ROOT);
	if (!can_make_cgroup_dir) {
		dprintf(D_ALWAYS, "Cannot mkdir %s, failing to use cgroups\n", leaf.c_str());
		return false;
	}

	// Now move pid to the leaf of the newly-created tree
	stdfs::path procs_filename = leaf / "cgroup.procs";
	int fd = open(procs_filename.c_str(), O_WRONLY, 0666);
	if (fd > 0) {
		char buf[16];
		sprintf(buf, "%u", pid);
		int r = write(fd, buf, strlen(buf));
		if (r < 0) {
			dprintf(D_ALWAYS, "Error writing procid %d to %s: %s\n", pid, procs_filename.c_str(), strerror(errno));
			close(fd);
			return false;
		}
		close(fd);
	}

	// Set limits, if any
	if (cgroup_memory_limit > 0) {
		// write memory limits
		stdfs::path memory_limits_path = leaf / "memory.max";
		int fd = open(memory_limits_path.c_str(), O_WRONLY, 0666);
		if (fd > 0) {
			char buf[16];
			sprintf(buf, "%lu", cgroup_memory_limit);
			int r = write(fd, buf, strlen(buf));
			if (r < 0) {
				dprintf(D_ALWAYS, "Error setting cgroup memory limit of %s in cgroup %s: %s\n", buf, leaf.c_str(), strerror(errno));
			}
			close(fd);
		} else {
			dprintf(D_ALWAYS, "Error setting cgroup memory limit of %lu in cgroup %s: %s\n", cgroup_memory_limit, leaf.c_str(), strerror(errno));
		}
	}

	// Set cpu limits, if any
	if (cgroup_cpu_shares > 0) {
		// write memory limits
		stdfs::path cpu_shares_path = leaf / "cpu.weight";
		int fd = open(cpu_shares_path.c_str(), O_WRONLY, 0666);
		if (fd > 0) {
			char buf[16];
			sprintf(buf, "%d", cgroup_cpu_shares);
			int r = write(fd, buf, strlen(buf));
			if (r < 0) {
				dprintf(D_ALWAYS, "Error setting cgroup cpu weight of %d in cgroup %s: %s\n", cgroup_cpu_shares, leaf.c_str(), strerror(errno));
			}
			close(fd);
		} else {
			dprintf(D_ALWAYS, "Error setting cgroup cpu weight of %d in cgroup %s: %s\n", cgroup_cpu_shares, leaf.c_str(), strerror(errno));
		}
	}

	// If this fails, we will run without oom killing, which is unfortunate, but
	// we made it decades without this support
	stdfs::path oom_group = cgroup_mount_point() / stdfs::path(cgroup_name) / "memory.oom.group";
	fd = open(oom_group.c_str(), O_WRONLY, 0666);
	if (fd > 0) {
		char one[1] = {'1'};
		ssize_t result = write(fd, one, 1);
		if (result < 0) {
			dprintf(D_ALWAYS, "Error enabling per-cgroup oom killing: %d (%s)\n", errno, strerror(errno));
		}
		close(fd);
	} else {
		dprintf(D_ALWAYS, "Error enabling per-cgroup oom killing: %d (%s)\n", errno, strerror(errno));
	}

	return true;
}

bool 
ProcFamilyDirectCgroupV2::track_family_via_cgroup(pid_t pid, const FamilyInfo *fi) {

	ASSERT(fi->cgroup);

	std::string cgroup_name = fi->cgroup;
	this->cgroup_memory_limit = fi->cgroup_memory_limit;
	this->cgroup_cpu_shares = fi->cgroup_cpu_shares;

	auto [it, success] = cgroup_map.insert(std::make_pair(pid, cgroup_name));
	if (!success) {
		ASSERT("Couldn't insert into cgroup map, duplicate?");
	}

	return cgroupify_process(cgroup_name, pid);

	return true;
}

bool
ProcFamilyDirectCgroupV2::get_usage(pid_t pid, ProcFamilyUsage& usage, bool /*full*/)
{
	// DaemonCore uses "get_usage(getpid())" to test the procd, ignoring the usage
	// even if we haven't registered that pid as a subfamily.  Or even if there
	// is a procd.

	if (pid == getpid()) {
		return true;
	}

	std::string cgroup_name = cgroup_map[pid];

	// Initialize the ones we don't set to -1 to mean "don't know".
	usage.block_reads = usage.block_writes = usage.block_read_bytes = usage.block_write_bytes = usage.m_instructions = -1;
	usage.io_wait = -1.0;
	usage.total_proportional_set_size_available = false;
	usage.total_proportional_set_size = 0;

	stdfs::path cgroup_root_dir = cgroup_mount_point();
	stdfs::path leaf            = cgroup_root_dir / cgroup_name;
	stdfs::path cpu_stat        = leaf / "cpu.stat";

	// Get cpu statistics from cpu.stat  Format is
	//
	// cpu.stat:
	// usage_usec 8691663872
	// user_usec 1445107847
	// system_usec 7246556025

	FILE *f = fopen(cpu_stat.c_str(), "r");
	if (!f) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::get_usage cannot open %s: %d %s\n", cpu_stat.c_str(), errno, strerror(errno));
		return false;
	}

	uint64_t user_usec = 0;
	uint64_t sys_usec  = 0;

	char word[128]; // max size of a word in cpu.stat
	while (fscanf(f, "%s", word) != EOF) {
		// if word is usage_usec
		if (strcmp(word, "user_usec") == 0) {
			// next word is the user time in microseconds
			if (fscanf(f, "%ld", &user_usec) != 1) {
				dprintf(D_ALWAYS, "Error reading user_usec field out of cpu.stat\n");
				fclose(f);
				return false;
			}
		}
		if (strcmp(word, "system_usec") == 0) {
			// next word is the system time in microseconds
			if (fscanf(f, "%ld", &sys_usec) != 1) {
				dprintf(D_ALWAYS, "Error reading system_usec field out of cpu.stat\n");
				fclose(f);
				return false;
			}
		}
	}
	fclose(f);

	time_t wall_time = time(nullptr) - start_time;
	usage.percent_cpu = double(user_usec + sys_usec) / double((wall_time * 1'000'000));

	usage.user_cpu_time = user_usec / 1'000'000; // usage.user_cpu_times in seconds, ugh
	usage.sys_cpu_time  =  sys_usec / 1'000'000; //  usage.sys_cpu_times in seconds, ugh
	
	stdfs::path memory_current = leaf / "memory.current";
	stdfs::path memory_peak   = leaf / "memory.peak";

	f = fopen(memory_current.c_str(), "r");
	if (!f) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::get_usage cannot open %s: %d %s\n", memory_current.c_str(), errno, strerror(errno));
		return false;
	}

	uint64_t memory_current_value = 0;
	if (fscanf(f, "%ld", &memory_current_value) != 1) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::get_usage cannot read %s: %d %s\n", memory_current.c_str(), errno, strerror(errno));
		fclose(f);
		return false;
	}
	fclose(f);

	uint64_t memory_peak_value = 0;

	f = fopen(memory_peak.c_str(), "r");
	if (!f) {
		// Some cgroup v2 versions don't have this file
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::get_usage cannot open %s: %d %s\n", memory_peak.c_str(), errno, strerror(errno));
	} else {
		if (fscanf(f, "%ld", &memory_peak_value) != 1) {
	
			// But this error should never happen
			dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::get_usage cannot read %s: %d %s\n", memory_peak.c_str(), errno, strerror(errno));
			fclose(f);
			return false;
		}
		fclose(f);
	}

	// usage is in kbytes.  cgroups reports in bytes
	usage.total_image_size = usage.total_resident_set_size = (memory_current_value / 1024);

	// Sanity check if system is missing memory.peak file
	if (memory_current_value > memory_peak_value) {
		memory_peak_value = memory_current_value;
	}

	// More sanity checking to latch memory high water mark
	if ((memory_peak_value / 1024) > usage.max_image_size) {
		usage.max_image_size = memory_peak_value / 1024;
	}

	return true;
}

// Note that in cgroup v2, cgroup.procs contains only those processes in
// this direct cgroup, and does not contain processes in any descendent
// cgroup (except the / croup, which does).
bool
ProcFamilyDirectCgroupV2::signal_process(pid_t pid, int sig)
{
	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::signal_process for %u sig %d\n", pid, sig);

	std::string cgroup_name = cgroup_map[pid];

	pid_t me = getpid();
	stdfs::path procs = cgroup_mount_point() / stdfs::path(cgroup_name) / stdfs::path("cgroup.procs");

	TemporaryPrivSentry sentry(PRIV_ROOT);
	FILE *f = fopen(procs.c_str(), "r");
	if (!f) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::signal_process cannot open %s: %d %s\n", procs.c_str(), errno, strerror(errno));
		return false;
	}
	pid_t victim_pid;
	while (fscanf(f, "%d", &victim_pid) != EOF) {
		if (pid != me) { // just in case
			kill(victim_pid, sig);
		}
	}
	fclose(f);
	return true;
}

// Writing a '1' to cgroup.freeze freezes all the processes in this cgrup
// and also freezes all descendent cgroups.  Might need to poll
// cgroup.freeze until it reads '1' to verify that everyone is frozen.
bool
ProcFamilyDirectCgroupV2::suspend_family(pid_t pid)
{
	std::string cgroup_name = cgroup_map[pid];

	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::suspend for pid %u for root pid %u in cgroup %s\n", 
			pid, family_root_pid, cgroup_name.c_str());

	stdfs::path freezer = cgroup_mount_point() / stdfs::path(cgroup_name) / "cgroup.freeze";
	bool success = true;
	TemporaryPrivSentry sentry( PRIV_ROOT );
	int fd = open(freezer.c_str(), O_WRONLY, 0666);
	if (fd > 0) {

		char one[1] = {'1'};
		ssize_t result = write(fd, one, 1);
		if (result < 0) {
			dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::suspend_family error %d (%s) writing to cgroup.freeze\n", errno, strerror(errno));
			success = false;
		}
		close(fd);
	} else {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::suspend_family error %d (%s) opening cgroup.freeze\n", errno, strerror(errno));
		success = false;
	}
	return success;
}

bool
ProcFamilyDirectCgroupV2::continue_family(pid_t pid)
{
	std::string cgroup_name = cgroup_map[pid];
	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::continue for pid %u for root pid %u in cgroup %s\n", 
			pid, family_root_pid, cgroup_name.c_str());

	stdfs::path freezer = cgroup_mount_point() / stdfs::path(cgroup_name) / "cgroup.freeze";
	bool success = true;
	TemporaryPrivSentry sentry( PRIV_ROOT );
	int fd = open(freezer.c_str(), O_WRONLY, 0666);
	if (fd > 0) {

		char zero[1] = {'0'};
		ssize_t result = write(fd, zero, 1);
		if (result < 0) {
			dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::continue_family error %d (%s) writing to cgroup.freeze\n", errno, strerror(errno));
			success = false;
		}
		close(fd);
	} else {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::continue_family error %d (%s) opening cgroup.freeze\n", errno, strerror(errno));
		success = false;
	}

	return success;
}

bool
ProcFamilyDirectCgroupV2::kill_family(pid_t pid)
{
	std::string cgroup_name = cgroup_map[pid];

	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::kill_family for pid %u\n", pid);

	// Suspend the whole cgroup first, so that all processes are atomically
	// killed
	this->suspend_family(pid);
	this->signal_process(pid, SIGKILL);
	this->continue_family(pid);
	return true;
}

// Note: DaemonCore doesn't call this from the starter, because
// the starter exits from the JobReaper, and dc call this after
// calling the reaper.
bool
ProcFamilyDirectCgroupV2::unregister_family(pid_t pid)
{
	std::string cgroup_name = cgroup_map[pid];

	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::unregister_family for pid %u\n", pid);
	// Remove this cgroup, so that we clear the various peak statistics it holds
	
	// TODO: should recursively remove all descendent directories
	TemporaryPrivSentry sentry(PRIV_ROOT);
	int r = rmdir((cgroup_mount_point() / cgroup_name).c_str());
	if (r < 0) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::unregister_family error removing cgroup %s: %s\n", cgroup_name.c_str(), strerror(errno));
	}

	return true;
}

bool 
ProcFamilyDirectCgroupV2::has_been_oom_killed(pid_t pid) {
	bool killed = false;

	std::string cgroup_name = cgroup_map[pid];

	stdfs::path cgroup_root_dir = cgroup_mount_point();
	stdfs::path leaf            = cgroup_root_dir / cgroup_name;
	stdfs::path memory_events   = leaf / "memory.events"; // includes children, if any

	dprintf(D_FULLDEBUG, "ProcFamilyDirectCgroupV2::checking if pid %u was oom killed... \n", pid);
	FILE *f = fopen(memory_events.c_str(), "r");
	if (!f) {
		dprintf(D_ALWAYS, "ProcFamilyDirectCgroupV2::has_been_oom_killed cannot open %s: %d %s\n", memory_events.c_str(), errno, strerror(errno));
		return false;
	}

	uint64_t oom_count = 0;

	char word[128]; // max size of a word in memory_events
	while (fscanf(f, "%s", word) != EOF) {
		// if word is oom_killed
		if (strcmp(word, "oom_group_kill") == 0) {
			// next word is the count
			if (fscanf(f, "%ld", &oom_count) != 1) {
				dprintf(D_ALWAYS, "Error reading oom_count field out of cpu.stat\n");
				fclose(f);
				return false;
			}
		}
	}
	fclose(f);

	killed = oom_count > 0;

	return killed;
}

// Returns true if cgroup v2 is mounted
bool 
ProcFamilyDirectCgroupV2::has_cgroup_v2() {
	bool found = false;
	stdfs::path mount_point = cgroup_mount_point();

	// if cgroup.procs exists in the root, then we are
	// a pure cgroup v2 system

	// Don't bother to check with elevated privileges
	stdfs::path cgroup_root_procs = mount_point / "cgroup.procs";
	std::error_code ec;
	if (stdfs::exists(cgroup_root_procs, ec)) {
		found = true;
	}
	return found;
}

bool 
ProcFamilyDirectCgroupV2::can_create_cgroup_v2() {
	bool success = false;

	if (!has_cgroup_v2()) {
		return false;
	}

	TemporaryPrivSentry sentry(PRIV_ROOT);
	if (access(cgroup_mount_point().c_str(), R_OK | W_OK) == 0) {
		success = true;
	}
	return success;
}
