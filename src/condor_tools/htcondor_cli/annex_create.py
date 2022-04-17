#!/usr/bin/env python3


import os
import sys
import fcntl
import atexit
import signal
import getpass
import logging
import secrets
import textwrap
import subprocess
from pathlib import Path

import htcondor

#
# We could try to import colorama here -- see condor_watch_q -- but that's
# only necessary for Windows.
#

INITIAL_CONNECTION_TIMEOUT = int(htcondor.param.get("ANNEX_INITIAL_CONNECTION_TIMEOUT", 180))
REMOTE_CLEANUP_TIMEOUT = int(htcondor.param.get("ANNEX_REMOTE_CLEANUP_TIMEOUT", 60))
REMOTE_MKDIR_TIMEOUT = int(htcondor.param.get("ANNEX_REMOTE_MKDIR_TIMEOUT", 30))
REMOTE_POPULATE_TIMEOUT = int(htcondor.param.get("ANNEX_REMOTE_POPULATE_TIMEOUT", 60))
TOKEN_FETCH_TIMEOUT = int(htcondor.param.get("ANNEX_TOKEN_FETCH_TIMEOUT", 20))

#
# For queues with the whole-node allocation policy, cores and RAM -per-node
# are only informative at the moment; we could compute the necessary number
# of nodes for the user (as long as we echo it back to them), but let's not
# do that for now and just tell the user to size their requests with the
# units appropriate to the queue.
#
MACHINE_TABLE = {

    # The key is currently both the value of the command-line option
    # and part of the name of some files in condor_scripts/annex.  This
    # shouldn't be hard to change.
    "stampede2": {
        "pretty_name":      "Stampede 2",
        "gsissh_name":      "stampede2",
        "default_queue":    "normal",

        # This isn't all of the queues on Stampede 2, but we
        # need to think about what it means, if anything, if
        # recognize certain queues.  For now, these are the
        # only three queues I've actually tested.
        "queues": {
            "normal": {
                "max_nodes_per_job":    256,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       68,

                "max_jobs_in_queue":    50,
            },
            "development": {
                "max_nodes_per_job":    16,
                "max_duration":         2 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       68,

                "max_jobs_in_queue":    1,
            },
            "skx-normal": {
                "max_nodes_per_job":    128,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       48,

                "max_jobs_in_queue":    20,
            },
        },
    },

    "expanse": {
        "pretty_name":      "Expanse",
        "gsissh_name":      "expanse",
        "default_queue":    "compute",

        # GPUs are completed untested, see above.
        "queues": {
            "compute": {
                "max_nodes_per_job":    32,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       128,
                "ram_per_node":         256,

                "max_jobs_in_queue":    64,
            },
            "gpu": {
                "max_nodes_per_job":    4,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       40,
                "ram_per_node":         256,

                "max_jobs_in_queue":    8,
            },
            "shared": {
                "max_nodes_per_job":    1,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "cores_or_ram",
                "cores_per_node":       128,
                "ram_per_node":         256,

                "max_jobs_in_queue":    4096,
            },
            "gpu-shared": {
                "max_nodes_per_job":    1,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "cores_or_ram",
                "cores_per_node":       40,
                "ram_per_node":         384,

                "max_jobs_in_queue":    24,
            },
        },
    },

    "bridges2": {
        "pretty_name":      "Bridges-2",
        "gsissh_name":      "bridges2",
        "default_queue":    "RM",

        # Omitted the GPU queues because they are based on a different set of parameters.
        # Queue limits are not documented, possibly nonexistent.
        # XXX You don't request memory for Bridges2; should we do a "ram_per_core" instead?
        "queues": {
            "RM": {
                "max_nodes_per_job":    50,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       128,
                "ram_per_node":         (253000 // 1024),

                "max_jobs_in_queue":    50,
            },
            "RM-512": {
                "max_nodes_per_job":    2,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "node",
                "cores_per_node":       128,
                "ram_per_node":         (515000 // 1024),

                "max_jobs_in_queue":    50,
            },
            "RM-shared": {
                "max_nodes_per_job":    1,
                "max_duration":         48 * 60 * 60,
                "allocation_type":      "cores_or_ram",
                # RM-shared lets you request up to half an RM node
                "cores_per_node":       64,
                "ram_per_node":         (253000 // 2 // 1024),

                "max_jobs_in_queue":    50,
            },
            "EM": {
                "max_nodes_per_job":    2,
                "max_duration":         120 * 60 * 60,
                "allocation_type":      "cores_or_ram",
                "cores_per_node":       96,
                # The EM queue specifies "MaxMemPerCPU"
                "ram_per_node":         (42955 * 96 // 1024),

                "max_jobs_in_queue":    50,
            },
        },
    },
}


def make_initial_ssh_connection(
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
):
    proc = subprocess.Popen(
        [
            "ssh",
            "-f",
            *ssh_connection_sharing,
            ssh_target,
            *ssh_indirect_command,
            "exit",
            "0",
        ],
    )

    try:
        return proc.wait(timeout=INITIAL_CONNECTION_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise RuntimeError(
            f"Did not make initial connection after {INITIAL_CONNECTION_TIMEOUT} seconds, aborting."
        )
    except KeyboardInterrupt:
        sys.exit(1)


def remove_remote_temporary_directory(
    logger, ssh_connection_sharing, ssh_target, ssh_indirect_command, remote_script_dir
):
    if remote_script_dir is not None:
        logger.debug("Cleaning up remote temporary directory...")
        proc = subprocess.Popen(
            [
                "ssh",
                *ssh_connection_sharing,
                ssh_target,
                *ssh_indirect_command,
                "rm",
                "-fr",
                remote_script_dir,
            ],
        )

        try:
            return proc.wait(timeout=REMOTE_CLEANUP_TIMEOUT)
        except subprocess.TimeoutExpired:
            logger.error(
                f"Did not clean up remote temporary directory after {REMOTE_CLEANUP_TIMEOUT} seconds, '{remote_script_dir}' may need to be deleted manually."
            )

    return 0


def make_remote_temporary_directory(
    logger,
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
):
    remote_command = r'mkdir -p \${HOME}/.hpc-annex/scratch && ' \
        r'mktemp --tmpdir=\${HOME}/.hpc-annex/scratch --directory remote_script.XXXXXXXX'
    proc = subprocess.Popen(
        [
            "ssh",
            *ssh_connection_sharing,
            ssh_target,
            *ssh_indirect_command,
            "sh",
            "-c",
            f"\"'{remote_command}'\"",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        # This implies text mode.
        errors="replace",
    )

    try:
        out, err = proc.communicate(timeout=REMOTE_MKDIR_TIMEOUT)

        if proc.returncode == 0:
            return out.strip()
        else:
            logger.error("Failed to make remote temporary directory, got output:")
            logger.error(f"{out.strip()}")
            raise IOError("Failed to make remote temporary directory.")

    except subprocess.TimeoutExpired:
        logger.error(
            f"Failed to make remote temporary directory after {REMOTE_MKDIR_TIMEOUT} seconds, aborting..."
        )
        logger.info("... cleaning up...")
        proc.kill()
        logger.debug("...")
        out, err = proc.communicate()
        logger.info("... clean up complete.")
        raise IOError(
            f"Failed to make remote temporary directory after {REMOTE_MKDIR_TIMEOUT} seconds"
        )


def transfer_sif_files(
    logger,
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
    remote_script_dir,
    sif_files,
):
    # We'd prefer to avoid the possibility of .sif name collisions,
    # but for now that's too hard.  FIXME: detect it?
    # Instead, we'll rewrite ContainerImage to be the basename in the
    # job ad, assuming that TransferInput already has the correct path.
    file_list = [f"-C {str(sif_file.parent)} {sif_file.name}" for sif_file in sif_files]
    files = " ".join(file_list)

    # -h meaning "follow symlinks"
    files = f"-h {files}"

    # Meaning, "stuff these files into the sif/ directory."
    files = f"--transform='s|^|sif/|' {files}"

    transfer_files(
        logger,
        ssh_connection_sharing,
        ssh_target,
        ssh_indirect_command,
        remote_script_dir,
        files,
        "transfer .sif files",
    )


def populate_remote_temporary_directory(
    logger,
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
    target,
    local_script_dir,
    remote_script_dir,
    token_file,
    password_file,
):
    files = [
        f"-C {local_script_dir} {target}.sh",
        f"-C {local_script_dir} {target}.pilot",
        f"-C {local_script_dir} {target}.multi-pilot",
        f"-C {str(token_file.parent)} {token_file.name}",
        f"-C {str(password_file.parent)} {password_file.name}",
    ]
    files = " ".join(files)

    transfer_files(
        logger,
        ssh_connection_sharing,
        ssh_target,
        ssh_indirect_command,
        remote_script_dir,
        files,
        "populate remote temporary directory",
    )


def transfer_files(
    logger,
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
    remote_script_dir,
    files,
    task,
):
    # FIXME: Pass an actual list to Popen
    full_command = f'tar -c -f- {files} | ssh {" ".join(ssh_connection_sharing)} {ssh_target} {" ".join(ssh_indirect_command)} tar -C {remote_script_dir} -x -f-'
    proc = subprocess.Popen(
        [
            full_command
        ],
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        # This implies text mode.
        errors="replace",
    )

    try:
        out, err = proc.communicate(timeout=REMOTE_POPULATE_TIMEOUT)

        if proc.returncode == 0:
            return 0
        else:
            logger.error(f"Failed to {task}, aborting.")
            logger.debug(f"Command '{full_command}' got output:")
            logger.warning(f"{out.strip()}")
            raise RuntimeError(f"Failed to {task}")

    except subprocess.TimeoutExpired:
        logger.error(
            f"Failed to {task} in {REMOTE_POPULATE_TIMEOUT} seconds, aborting."
        )
        proc.kill()
        out, err = proc.communicate()
        raise RuntimeError(f"Failed to {task} in {REMOTE_POPULATE_TIMEOUT} seconds")


def extract_full_lines(buffer):
    last_newline = buffer.rfind(b"\n")
    if last_newline == -1:
        return buffer, []

    lines = [line.decode("utf-8", errors="replace")
             for line in buffer[:last_newline].split(b"\n")]
    return buffer[last_newline + 1 :], lines


def process_line(line, update_function):
    control = "=-.-= "
    if line.startswith(control):
        command = line[len(control) :]
        attribute, value = command.split(" ")
        update_function(attribute, value)
    else:
        print("   ", line)


def invoke_pilot_script(
    ssh_connection_sharing,
    ssh_target,
    ssh_indirect_command,
    remote_script_dir,
    target,
    annex_name,
    queue_name,
    collector,
    token_file,
    lifetime,
    owners,
    nodes,
    allocation,
    update_function,
    request_id,
    password_file,
    cpus,
    mem_mb,
):
    args = [
        "ssh",
        *ssh_connection_sharing,
        ssh_target,
        *ssh_indirect_command,
        str(remote_script_dir / f"{target}.sh"),
        annex_name,
        queue_name,
        collector,
        str(remote_script_dir / token_file.name),
        str(lifetime),
        str(remote_script_dir / f"{target}.pilot"),
        owners,
        str(nodes),
        str(remote_script_dir / f"{target}.multi-pilot"),
        str(allocation),
        request_id,
        str(remote_script_dir / password_file.name),
        str(cpus),
        str(mem_mb),
    ]
    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    #
    # An alternative to nonblocking I/O and select.poll() would be to
    # spawn a thread to do blocking read()s and feed a queue.Queue;
    # the main thread would use queue.Queue.get_nowait().
    #
    # This should be pretty safe and easy to implement, but let's not
    # bother unless we need to (because we lose data while processing).
    #

    # Make the output stream nonblocking.
    out_fd = proc.stdout.fileno()
    flags = fcntl.fcntl(out_fd, fcntl.F_GETFL)
    fcntl.fcntl(out_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    # Check to see if the child process is still alive.  Regardless, empty
    # the output stream, then process it.  If the child process was not alive,
    # we've read all of its output and are done.  Otherwise, repeat.

    out_bytes = bytes()
    out_buffer = bytes()

    while True:
        rc = proc.poll()

        while True:
            try:
                out_bytes = os.read(proc.stdout.fileno(), 1024)
                if len(out_bytes) == 0:
                    break
                out_buffer += out_bytes

                # Process the output a line at a time, since the control
                # sequences we care about are newline-terminated.

                out_buffer, lines = extract_full_lines(out_buffer)
                for line in lines:
                    process_line(line, update_function)

            # Python throws a BlockingIOError if an asynch FD is unready.
            except BlockingIOError:
                pass

        if rc != None:
            break

    # Print the remainder, if any.
    if len(out_buffer) != 0:
        print("   ", out_buffer.decode("utf-8", errors="replace"))
    print()

    # Set by proc.poll().
    return proc.returncode


# FIXME: catch HTCondorIOError and retry.
def updateJobAd(cluster_id, attribute, value, remotes):
    schedd = htcondor.Schedd()
    schedd.edit(cluster_id, f"hpc_annex_{attribute}", f'"{value}"')
    remotes[attribute] = value


def extract_sif_file(job_ad):
    try:
        container_image = Path(job_ad.get("ContainerImage"))
    except TypeError:
        return None

    if not container_image.name.endswith(".sif"):
        return None

    if container_image.is_absolute():
        return container_image

    try:
        iwd = Path(job_ad["iwd"])
    except KeyError:
        raise KeyError("Could not find iwd in job ad.")

    return iwd / container_image


def create_annex_token(logger, type):
    token_lifetime = int(htcondor.param.get("ANNEX_TOKEN_LIFETIME", 60 * 60 * 24 * 90))
    annex_token_key_name = htcondor.param.get("ANNEX_TOKEN_KEY_NAME", "hpcannex-key")
    annex_token_domain = htcondor.param.get("ANNEX_TOKEN_DOMAIN", "annex.osgdev.chtc.io")
    token_name = f"{type}.{getpass.getuser()}@{annex_token_domain}"

    args = [
        'condor_token_fetch',
        '-lifetime', str(token_lifetime),
        '-token', token_name,
        '-key', annex_token_key_name,
        '-authz', 'READ',
        '-authz', 'ADVERTISE_STARTD',
        '-authz', 'ADVERTISE_MASTER',
        '-authz', 'ADVERTISE_SCHEDD',
    ]

    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        # This implies text mode.
        errors="replace",
    )

    try:
        out, err = proc.communicate(timeout=TOKEN_FETCH_TIMEOUT)

        if proc.returncode == 0:
            sec_token_directory = htcondor.param.get("SEC_TOKEN_DIRECTORY")
            if sec_token_directory == "":
                sec_token_directory = "~/.condor/tokens.d"
            return os.path.expanduser(f"{sec_token_directory}/{token_name}")
        else:
            logger.error(f"Failed to create annex token, aborting.")
            logger.warning(f"{out.strip()}")
            raise RuntimeError(f"Failed to create annex token")

    except subprocess.TimeoutExpired:
        logger.error(f"Failed to create annex token in {TOKEN_FETCH_TIMEOUT} seconds, aborting.")
        proc.kill()
        out, err = proc.communicate()
        raise RuntimeError(f"Failed to create annex token in {TOKEN_FETCH_TIMEOUT} seconds")


def annex_inner_func(
    logger,
    annex_name,
    nodes,
    lifetime,
    allocation,
    queue_at_machine,
    owners,
    collector,
    token_file,
    password_file,
    ssh_target,
    control_path,
    cpus,
    mem_mb,
):
    if '@' in queue_at_machine:
        (queue_name, target) = queue_at_machine.split('@', 1)
    else:
        error_string = "Target must have the form queue@machine."

        target = queue_at_machine.casefold()
        if target not in MACHINE_TABLE:
            error_string = f"{error_string}  Also, '{queue_at_machine}' is not a known machine."
        else:
            default_queue = MACHINE_TABLE[target]['default_queue']
            queue_list = "\n    ".join([q for q in MACHINE_TABLE[target]['queues']])
            error_string = f"{error_string}  Supported queues are:\n    {queue_list}\nUse '{default_queue}' if you're not sure."

        raise ValueError(error_string)

    #
    # We'll need to validate the requested nodes (or CPUs and memory)
    # against the requested queue[-machine pair].  We also need to
    # check the lifetime (and idle time, once we support it).  Once
    # all of that's been validated, we should print something like:
    #
    #   Once established by Stampede 2, the annex will be available to
    #   run your jobs for no more than 48 hours (use --duration to change).
    #   It will self-destruct after 20 minutes of inactivity (use
    #   --idle to change).
    #
    # .. although it should maybe also include the queue name and size.
    #

    # We use this same method to determine the user name in `htcondor job`,
    # so even if it's wrong, it will at least consistently so.
    username = getpass.getuser()

    target = target.casefold()
    if target not in MACHINE_TABLE:
        raise ValueError(f"{target} is not a known machine.")

    # Location of the local universe script files
    local_script_dir = (
        Path(htcondor.param.get("LIBEXEC", "/usr/libexec/condor")) / "annex"
    )

    if not local_script_dir.is_dir():
        raise RuntimeError(f"Annex script dir {local_script_dir} not found or not a directory.")

    # If the user didn't specify a token file, create one on the fly.
    if token_file is None:
        logger.debug("Creating annex token...")
        token_file = create_annex_token(logger, annex_name)
        atexit.register(lambda: os.unlink(token_file))
        logger.debug("..done.")

    token_file = Path(token_file).expanduser()
    if not token_file.exists():
        raise RuntimeError(f"Token file {token_file} doesn't exist.")

    control_path = Path(control_path).expanduser()
    if control_path.is_dir():
        if not control_path.exists():
            logger.debug(f"{control_path} not found, attempt to create it")
            control_path.mkdir(parents=True, exist_ok=True)
    else:
        raise RuntimeError(f"{control_path} must be a directory")

    password_file = Path(password_file).expanduser()
    if not password_file.exists():
        try:
            old_umask = os.umask(0o077)
            with password_file.open("wb") as f:
                password = secrets.token_bytes(16)
                f.write(password)
            password_file.chmod(0o0400)
            try:
                os.umask(old_umask)
            except OSError:
                pass
        except OSError as ose:
            raise RuntimeError(
                f"Password file {password_file} does not exist and could not be created: {ose}."
            )

    # Derived constants.
    ssh_connection_sharing = [
        "-o",
        'ControlPersist="5m"',
        "-o",
        'ControlMaster="auto"',
        "-o",
        f'ControlPath="{control_path}/master-%C"',
    ]
    ssh_indirect_command = ["gsissh", MACHINE_TABLE[target]["gsissh_name"]]

    ##
    ## While we're requiring that jobs are submitted before creating the
    ## annex (for .sif pre-staging purposes), refuse to make the annex
    ## if no such jobs exist.
    ##
    schedd = htcondor.Schedd()
    annex_jobs = schedd.query(f'TargetAnnexName == "{annex_name}"')
    if len(annex_jobs) == 0:
        raise RuntimeError(
            f"No jobs for '{annex_name}' are in the queue. Use 'htcondor job submit --annex-name' to add them first."
        )
    logger.debug(
        f"""Found {len(annex_jobs)} annex jobs matching 'TargetAnnexName == "{annex_name}"."""
    )

    # Extract the .sif file from each job.
    sif_files = set()
    for job_ad in annex_jobs:
        sif_file = extract_sif_file(job_ad)
        if sif_file is not None:
            sif_file = Path(sif_file)
            if sif_file.exists():
                sif_files.add(sif_file)
            else:
                raise RuntimeError(
                    f"""Job {job_ad["ClusterID"]}.{job_ad["ProcID"]} specified container image '{sif_file}', which doesn't exist."""
                )
    if sif_files:
        logger.debug(f"Got sif files: {sif_files}")
    else:
        logger.debug("No sif files found, continuing...")
    # The .sif files will be transferred to the target machine later.

    # Distinguish our text from SSH's text.
    ANSI_BRIGHT = "\033[1m"
    ANSI_RESET_ALL = "\033[0m"

    ##
    ## The user will do the 2FA/SSO dance here.
    ##
    logger.info(
        f"{ANSI_BRIGHT}This command will access "
        f"{MACHINE_TABLE[target]['pretty_name']} via XSEDE.  To proceed, "
        f"enter your XSEDE user name and password at the prompt "
        f"below; to cancel, hit CTRL-C.{ANSI_RESET_ALL}"
    )
    logger.debug(
        f"  (You can run 'ssh {' '.join(ssh_connection_sharing)} {ssh_target}' to use the shared connection.)"
    )
    rc = make_initial_ssh_connection(
        ssh_connection_sharing,
        ssh_target,
        ssh_indirect_command,
    )
    if rc != 0:
        raise RuntimeError(
            f"Failed to make initial connection to {MACHINE_TABLE[target]['pretty_name']}, aborting ({rc})."
        )
    logger.info(f"{ANSI_BRIGHT}Thank you.{ANSI_RESET_ALL}\n")

    ##
    ## Register the clean-up function before creating the mess to clean-up.
    ##
    remote_script_dir = None

    # Allow atexit functions to run on SIGTERM.
    signal.signal(signal.SIGTERM, lambda signal, frame: sys.exit(128 + 15))
    # Hide the traceback on CTRL-C.
    signal.signal(signal.SIGINT, lambda signal, frame: sys.exit(128 + 2))
    # Remove the temporary directories on exit.
    atexit.register(
        lambda: remove_remote_temporary_directory(
            logger,
            ssh_connection_sharing,
            ssh_target,
            ssh_indirect_command,
            remote_script_dir,
        )
    )

    logger.debug("Making remote temporary directory...")
    remote_script_dir = Path(
        make_remote_temporary_directory(
            logger,
            ssh_connection_sharing,
            ssh_target,
            ssh_indirect_command,
        )
    )
    logger.debug(f"... made remote temporary directory {remote_script_dir} ...")

    logger.info(f"Populating annex temporary directory...")
    populate_remote_temporary_directory(
        logger,
        ssh_connection_sharing,
        ssh_target,
        ssh_indirect_command,
        target,
        local_script_dir,
        remote_script_dir,
        token_file,
        password_file,
    )
    if sif_files:
        logger.debug("... transferring container images ...")
        transfer_sif_files(
            logger,
            ssh_connection_sharing,
            ssh_target,
            ssh_indirect_command,
            remote_script_dir,
            sif_files,
        )
    logger.info("... populated.")

    # Submit local universe job.
    logger.debug("Submitting state-tracking job...")
    local_job_executable = local_script_dir / "annex-local-universe.py"
    if not local_job_executable.exists():
        raise RuntimeError(
            f"Could not find local universe executable, expected {local_job_executable}"
        )
    #
    # The magic in this job description is thus:
    #   * hpc_annex_start_time is undefined until the job runs and finds
    #     a machine ad with a matching hpc_annex_request_id.
    #   * The job will go idle (because it can't start) at that poing,
    #     based on its Requirements.
    #   * Before then, the job's on_exit_remove must be false -- not
    #     undefined -- to make sure it keeps polling.
    #   * The job runs every five minutes because of cron_minute.
    #
    submit_description = htcondor.Submit(
        {
            "universe": "local",
            # hpc_annex_start time is set by the job script when it finds
            # a machine with a matching request ID.  At that point, we can
            # stop runnig this script, but we don't remove it to simplify
            # the UI/UX code; instead, we wait until an hour past the end
            # of the request's lifetime to trigger a peridic remove.
            "requirements": "hpc_annex_start_time =?= undefined",
            "executable": str(local_job_executable),
            # Sadly, even if you set on_exit_remove to ! requirements,
            # the job lingers in X state for a good long time.
            "cron_minute": "*/5",
            "on_exit_remove": "PeriodicRemove =?= true",
            "periodic_remove": f"hpc_annex_start_time + {lifetime} + 3600 < time()",
            # Consider adding a log, an output, and an error file to assist
            # in debugging later.  Problem: where should it go?  How does it
            # cleaned up?
            "environment": f'PYTHONPATH={os.environ.get("PYTHONPATH", "")}',
            "+arguments": f'strcat( "$(CLUSTER).0 hpc_annex_request_id ", GlobalJobID, " {collector}")',
            "jobbatchname": f'{annex_name} [HPC Annex]',
            "+hpc_annex_request_id": 'GlobalJobID',
            # Properties of the annex request.  We should think about
            # representing these as a nested ClassAd.  Ideally, the back-end
            # would, instead of being passed a billion command-line arguments,
            # just pull this ad from the collector (after this local universe
            # job has forwarded it there).
            "+hpc_annex_name": f'"{annex_name}"',
            "+hpc_annex_queue_name": f'"{queue_name}"',
            "+hpc_annex_collector": f'"{collector}"',
            "+hpc_annex_lifetime": f'"{lifetime}"',
            "+hpc_annex_owners": f'"{owners}"',
            # FIXME: `nodes` should be undefined if not set on the
            # command line but either cpus or mem_mb are.
            "+hpc_annex_nodes": f'"{nodes}"'
                if nodes is not None else "undefined",
            "+hpc_annex_cpus": f'"{cpus}"'
                if cpus is not None else "undefined",
            "+hpc_annex_mem_mb": f'"{mem_mb}"'
                if mem_mb is not None else "undefined",
            "+hpc_annex_allocation": f'"{allocation}"'
                if allocation is not None else "undefined",
            # Hard state required for clean up.  We'll be adding
            # hpc_annex_PID, hpc_annex_PILOT_DIR, and hpc_annex_JOB_ID
            # as they're reported by the back-end script.
            "+hpc_annex_remote_script_dir": f'"{remote_script_dir}"',
        }
    )

    try:
        logger.debug(f"")
        logger.debug(textwrap.indent(str(submit_description), "  "))
        submit_result = schedd.submit(submit_description)
    except Exception:
        raise RuntimeError(f"Failed to submit state-tracking job, aborting.")

    cluster_id = submit_result.cluster()
    logger.debug(f"... done.")
    logger.debug(f"with cluster ID {cluster_id}.")

    results = schedd.query(
        f'ClusterID == {cluster_id} && ProcID == 0',
        opts=htcondor.QueryOpts.DefaultMyJobsOnly,
        projection=["GlobalJobID"],
        )
    request_id = results[0]["GlobalJobID"]

    ##
    ## We changed the job(s) at submit time to prevent them from running
    ## anywhere other than the annex, so it's OK to change them again to
    ## make it impossible to run them anywhere else before the annex job
    ## is successfully submitted.  Doing so after allows for a race
    ## condition, so let's not, since we don't hvae to.
    ##
    ## Change the jobs so that they don't transfer the .sif files we just
    ## pre-staged.
    ##
    ## The startd can't rewrite the job ad the shadow uses to decide
    ## if it should transfer the .sif file, but it can change ContainerImage
    ## to point the pre-staged image, if it's just the basename.  (Otherwise,
    ## it gets impossibly difficult to make the relative paths work.)
    ##
    ## We could do this at job-submission time, but then we'd have to record
    ## the full path to the .sif file in some other job attribute, so it's
    ## not worth changing at this point.
    ##
    for job_ad in annex_jobs:
        job_id = f'{job_ad["ClusterID"]}.{job_ad["ProcID"]}'
        sif_file = extract_sif_file(job_ad)
        if sif_file is not None:
            remote_sif_file = Path(sif_file).name
            logger.debug(
                f"Setting ContainerImage = {remote_sif_file} in annex job {job_id}"
            )
            schedd.edit(job_id, "ContainerImage", f'"{remote_sif_file}"')

            transfer_input = job_ad.get("TransferInput", "")
            input_files = transfer_input.split(",")
            if job_ad["ContainerImage"] in input_files:
                logger.debug(
                    f"Removing {job_ad['ContainerImage']} from input files in annex job {job_id}"
                )
                input_files.remove(job_ad["ContainerImage"])
                if len(input_files) != 0:
                    schedd.edit(job_id, "TransferInput", f'"{",".join(input_files)}"')
                else:
                    schedd.edit(job_id, "TransferInput", "undefined")

    remotes = {}
    logger.info(f"Requesting annex named '{annex_name}' from queue '{queue_name}' on {MACHINE_TABLE[target]['pretty_name']}...\n")
    rc = invoke_pilot_script(
        ssh_connection_sharing,
        ssh_target,
        ssh_indirect_command,
        remote_script_dir,
        target,
        annex_name,
        queue_name,
        collector,
        token_file,
        lifetime,
        owners,
        nodes,
        allocation,
        lambda attribute, value: updateJobAd(cluster_id, attribute, value, remotes),
        request_id,
        password_file,
        cpus,
        mem_mb,
    )

    if rc == 0:
        logger.info(f"... requested.")
        logger.info(f"\nIt may take some time for {MACHINE_TABLE[target]['pretty_name']} to establish the requested annex.")
        logger.info(f"To check on the status of the annex, run 'htcondor annex status {annex_name}'.")
    else:
        error = f"Failed to start annex, SLURM returned code {rc}"
        try:
            schedd.act(htcondor.JobAction.Remove, f'ClusterID == {cluster_id}', error)
        except Exception:
            logger.warn(f"Could not remove cluster ID {cluster_id}.")
        raise RuntimeError(error)


def annex_name_exists(annex_name):
    schedd = htcondor.Schedd()
    constraint = f'hpc_annex_name == "{annex_name}"'
    annex_jobs = schedd.query(
        constraint,
        opts=htcondor.QueryOpts.DefaultMyJobsOnly,
        projection=['ClusterID', 'ProcID'],
    )
    return len(annex_jobs) > 0


def annex_create(logger, annex_name, **others):
    if annex_name_exists(annex_name):
        raise ValueError(f"You've already created an annex named '{annex_name}'.  To request more resources, use 'htcondor annex add'.")
    return annex_inner_func(logger, annex_name, **others)


def annex_add(logger, annex_name, **others):
    if not annex_name_exists(annex_name):
        raise ValueError(f"You need to create an an annex named '{annex_name}' first.  To do so, use 'htcondor annex create'.")
    return annex_inner_func(logger, annex_name, **others)
