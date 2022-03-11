#!/bin/bash

CONTROL_PREFIX="=-.-="
echo "${CONTROL_PREFIX} PID $$"

function usage() {
    echo "Usage: ${0} \\"
    echo "       JOB_NAME QUEUE_NAME COLLECTOR TOKEN_FILE LIFETIME PILOT_BIN \\"
    echo "       OWNERS NODES MULTI_PILOT_BIN ALLOCATION REQUEST_ID PASSWORD_FILE"
    echo "where OWNERS is a comma-separated list"
}

JOB_NAME=$1
if [[ -z $JOB_NAME ]]; then
    usage
    exit 1
fi

QUEUE_NAME=$2
if [[ -z $QUEUE_NAME ]]; then
    usage
    exit 1
fi

COLLECTOR=$3
if [[ -z $COLLECTOR ]]; then
    usage
    exit 1
fi

TOKEN_FILE=$4
if [[ -z $TOKEN_FILE ]]; then
    usage
    exit 1
fi

LIFETIME=$5
if [[ -z $LIFETIME ]]; then
    usage
    exit 1
fi

PILOT_BIN=$6
if [[ -z $PILOT_BIN ]]; then
    usage
    exit 1
fi

OWNERS=$7
if [[ -z $OWNERS ]]; then
    usage
    exit 1
fi

NODES=$8
if [[ -z $NODES ]]; then
    usage
    exit 1
fi

MULTI_PILOT_BIN=$9
if [[ -z $MULTI_PILOT_BIN ]]; then
    usage
    exit 1
fi

ALLOCATION=${10}
if [[ $ALLOCATION == "None" ]]; then
    ALLOCATION=""
fi
if [[ -z $ALLOCATION ]]; then
    echo "Will try to use the default allocation."
fi

REQUEST_ID=${11}
if [[ -z $REQUEST_ID ]]; then
    usage
    exit 1
fi

PASSWORD_FILE=${12}
if [[ -z $PASSWORD_FILE ]]; then
    usage
    exit 1
fi

BIRTH=`date +%s`
echo "Starting script at `date`..."

#
# Download and configure the pilot on the head node before running it
# on the execute node(s).
#
# The following variables are constants.
#


# The binaries must be a tarball named condor-*, and unpacking that tarball
# must create a directory which also matches condor-*.
WELL_KNOWN_LOCATION_FOR_BINARIES=https://research.cs.wisc.edu/htcondor/tarball/current/9.5.4/update/condor-9.5.4-20220207-x86_64_CentOS7-stripped.tar.gz

# The configuration must be a tarball which does NOT match condor-*.  It
# will be unpacked in the root of the directory created by unpacking the
# binaries and as such should contain files in local/config.d/*.
WELL_KNOWN_LOCATION_FOR_CONFIGURATION=https://cs.wisc.edu/~tlmiller/hpc-config.tar.gz

# How early should HTCondor exit to make sure we have time to clean up?
CLEAN_UP_TIME=300

#
# Create pilot-specific directory on shared storage.  The least-awful way
# to do this is by having the per-node script NOT exec condor_master, but
# instead configure the condor_master to exit well before the "run time"
# of the job, and the script carry on to do the clean-up.
#
# That won't work for multi-node jobs, which we'll need eventually, but
# we'll leave that for then.
#

echo "Creating temporary directory for pilot..."
PILOT_DIR=`/usr/bin/mktemp --directory --tmpdir=${SCRATCH} 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to create temporary directory for pilot, aborting."
    echo ${PILOT_DIR}
    exit 1
fi
echo "${CONTROL_PREFIX} PILOT_DIR ${PILOT_DIR}"

function cleanup() {
    echo "Cleaning up temporary directory..."
    rm -fr ${PILOT_DIR}
}
trap cleanup EXIT

#
# Download the configuration.  (Should be smaller, and we fail if either
# of these downloads fail, so we may as well try this one first.)
#

cd ${PILOT_DIR}

# The .sif files need to live in ${PILOT_DIR} for the same reason.  We
# require that they have been transferred to the same directory as the
# PILOT_BIN mainly because this script has too many arguments already.
SIF_DIR=${PILOT_DIR}/sif
mkdir ${SIF_DIR}
PILOT_BIN_DIR=`dirname ${PILOT_BIN}`
mv ${PILOT_BIN_DIR}/sif ${PILOT_DIR}

# The pilot scripts need to live in the ${PILOT_DIR} because the front-end
# copied them into a temporary directory that it's responsible for cleaning up.
mv ${PILOT_BIN} ${PILOT_DIR}
mv ${MULTI_PILOT_BIN} ${PILOT_DIR}
PILOT_BIN=${PILOT_DIR}/`basename ${PILOT_BIN}`
MULTI_PILOT_BIN=${PILOT_DIR}/`basename ${MULTI_PILOT_BIN}`

echo "Downloading configuration..."
CONFIGURATION_FILE=`basename ${WELL_KNOWN_LOCATION_FOR_CONFIGURATION}`
CURL_LOGGING=`curl -fsSL ${WELL_KNOWN_LOCATION_FOR_CONFIGURATION} -o ${CONFIGURATION_FILE} 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to download configuration from '${WELL_KNOWN_LOCATION_FOR_CONFIGURATION}', aborting."
    echo ${CURL_LOGGING}
    exit 2
fi

#
# Download the binaries.
#
echo "Downloading binaries..."
BINARIES_FILE=`basename ${WELL_KNOWN_LOCATION_FOR_BINARIES}`
CURL_LOGGING=`curl -fsSL ${WELL_KNOWN_LOCATION_FOR_BINARIES} -o ${BINARIES_FILE} 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to download configuration from '${WELL_KNOWN_LOCATION_FOR_BINARIES}', aborting."
    echo ${CURL_LOGGING}
    exit 2
fi

#
# Unpack the binaries.
#
echo "Unpacking binaries..."
TAR_LOGGING=`tar -z -x -f ${BINARIES_FILE} 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to unpack binaries from '${BINARIES_FILE}', aborting."
    echo ${TAR_LOGGING}
    exit 3
fi

#
# Make the personal condor.
#
rm condor-*.tar.gz
cd condor-*

echo "Making a personal condor..."
MPC_LOGGING=`./bin/make-personal-from-tarball 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to make personal condor, aborting."
    echo ${MPC_LOGGING}
    exit 4
fi

#
# Create the script we need for Singularity.
#
# Unfortunately, the `module` command doesn't work without a bunch of
# random environmental set-up that's done when we're forking a process;
# for whatever reason, it's not good enough to run
# `module load tacc-singularity` before starting the master.  So
# we wanted to use a wrapper with `bash -l`.  That worked, but polluted the
# job's stderr with a message about a broken pipe.
#
# The problem is that /etc/profile.d/z00_tacc_login.sh runs a pipeline
# to determine the CPU model number that deliberately breaks the pipe;
# the awk scipt should not contain an 'exit' and instead the should have
# a '| head -n 1' at the end.
#
# It's not clear how one would run a command under `bash -l` and only
# get the command's standard error log.  It might be cleaner to depend
# on the error appearing rather than the following sequence, in which
# case we could ..?
#
# Yeah, screw all this for now.  We'll try to make TACC fix this broken
# script, instead.  I tested both the original line and the following one
# in the singularity.sh script, and the following line didn't have the error:
#
# model=$(awk -F : "/model/ { print \$2; exit }" /proc/cpuinfo | sed -e "s/ \*//g")
#
echo '#!/bin/bash
export USER=`/usr/bin/id -un`
export LD_PRELOAD=/opt/apps/xalt/xalt/lib64/libxalt_init.so
. /etc/tacc/tacc_functions &> /dev/null
. /etc/profile.d/z00_tacc_login.sh &> /dev/null
. /etc/profile.d/z01_lmod.sh &> /dev/null
module load tacc-singularity
exec singularity "$@"
' > ${PILOT_DIR}/singularity.sh
chmod 755 ${PILOT_DIR}/singularity.sh

# It may have take some time to get everything installed, so to make sure
# we get our full clean-up time, subtract off how long we've been running
# already.
YOUTH=$((`date +%s` - ${BIRTH}))
REMAINING_LIFETIME=$(((${LIFETIME} - ${YOUTH}) - ${CLEAN_UP_TIME}))

echo "Converting to a pilot..."
rm local/config.d/00-personal-condor
echo "
use role:execute
use security:recommended_v9_0
use feature:PartitionableSLot

COLLECTOR_HOST = ${COLLECTOR}

# We shouldn't ever actually need this, but it's convenient for testing.
SHARED_PORT_PORT = 0

# Allows condor_off (et alia) to work from the head node.
ALLOW_ADMINISTRATOR = \$(ALLOW_ADMINISTRATOR) $(whoami)@$(hostname)

# FIXME: use same-AuthenticatedIdentity once that becomes available, instead.
# Allows condor_off (et alia) to work from the submit node.
ALLOW_ADMINISTRATOR = \$(ALLOW_ADMINISTRATOR) condor_pool@*
SEC_DEFAULT_AUTHENTICATION_METHODS = FS IDTOKENS PASSWORD

# Eliminate a bogus, repeated warning in the logs.  This is a bug;
# it should be the default.
SEC_PASSWORD_DIRECTORY = \$(LOCAL_DIR)/passwords.d

# This is a bug; it should be the default.
SEC_TOKEN_SYSTEM_DIRECTORY = \$(LOCAL_DIR)/tokens.d
# Having to set it twice is also a bug.
SEC_TOKEN_DIRECTORY = \$(LOCAL_DIR)/tokens.d

# Don't run benchmarks.
RUNBENCHMARKS = FALSE

# We definitely need CCB.
CCB_ADDRESS = \$(COLLECTOR_HOST)

#
# Commit suicide after being idle for five minutes.
#
STARTD_NOCLAIM_SHUTDOWN = 300

#
# Don't run for more than two hours, to make sure we have time to clean up.
#
MASTER.DAEMON_SHUTDOWN_FAST = (CurrentTime - DaemonStartTime) > ${REMAINING_LIFETIME}

# Only start jobs from the specified owner.
START = \$(START) && stringListMember( Owner, \"${OWNERS}\" )

# Advertise the standard annex attributes (master ad for condor_off).
IsAnnex = TRUE
AnnexName = \"${JOB_NAME}\"
hpc_annex_request_id = \"${REQUEST_ID}\"
STARTD_ATTRS = \$(STARTD_ATTRS) AnnexName IsAnnex hpc_annex_request_id
MASTER_ATTRS = \$(MASTER_ATTRS) AnnexName IsAnnex hpc_annex_request_id

# Force all container-universe jobs to try to use pre-staged .sif files.
# This should be removed when we handle this in HTCondor proper.
JOB_EXECUTION_TRANSFORM_NAMES = siffile
JOB_EXECUTION_TRANSFORM_siffile @=end
if defined MY.ContainerImage
    EVALSET ContainerImage strcat(\"${SIF_DIR}/\", MY.ContainerImage)
endif
@end

#
# Subsequent configuration is machine-specific.
#

# This is made available via 'module load tacc-singularity', but the
# starter ignores PATH, so wrap it up.
SINGULARITY = ${PILOT_DIR}/singularity.sh

# Stampede 2 has Knight's Landing queues (4 threads per core) and Skylake
# queues (2 threads per core).  The "KNL" nodes have 68 cores and 96 GB
# of RAM; the "SKX" nodes have 48 cores and 192 GB of RAM.  It seems like
# the KNL cores are different-enough to justify a little judicious
# deception; since the SKX cores end up at 4 GB of RAM each, that seems
# reasonable (and it would be a pain to have different config for different
# queues).
COUNT_HYPERTHREAD_CPUS = FALSE

# Create dynamic slots 3 GB at a time.  This number was chosen because it's
# the amount of RAM requested per core on the OS Pool, but we actually bother
# to set it because we start seeing weird scaling issues with more than 64
# or so slots.  Since we can't fix that problem right now, avoid it.
MUST_MODIFY_REQUEST_EXPRS = TRUE
MODIFY_REQUEST_EXPR_REQUESTMEMORY = max({ 3072, quantize(RequestMemory, {128}) })

" > local/config.d/00-basic-pilot

mkdir local/passwords.d
mkdir local/tokens.d
mv ${TOKEN_FILE} local/tokens.d
mv ${PASSWORD_FILE} local/passwords.d/POOL

#
# Unpack the configuration on top.
#

echo "Unpacking configuration..."
TAR_LOGGING=`tar -z -x -f ../${CONFIGURATION_FILE} 2>&1`
if [[ $? != 0 ]]; then
    echo "Failed to unpack binaries from '${CONFIGURATION_FILE}', aborting."
    echo ${TAR_LOGGING}
    exit 5
fi

#
# Write the SLURM job.
#

# Compute the appropriate duration. (-t)
#
# This script does NOT embed knowledge about this machine's queue limits.  It
# seems like it'll be much easier to embed that knowledge in the UI script
# (rather than transmit a reasonable error back), plus it'll be more user-
# friendly, since they won't have to log in to get error about requesting
# the wrong queue length.
MINUTES=$(((${REMAINING_LIFETIME} + ${CLEAN_UP_TIME})/60))

# Request the appropriate number of nodes. (-N)
# Request the appropriate number of tasks. (-n)
#
# On Stampede 2, TACC allocates only whole nodes, so -N = ${NODES}.  Since
# this is not an MPI job, we want one task per node, and -n = ${NODES}.

if [[ -n $ALLOCATION ]]; then
    SBATCH_ALLOCATION_LINE="#SBATCH -A ${ALLOCATION}"
fi

echo '#!/bin/bash' > ${PILOT_DIR}/stampede2.slurm
echo "
#SBATCH -J ${JOB_NAME}
#SBATCH -o ${PILOT_DIR}/%j.out
#SBATCH -e ${PILOT_DIR}/%j.err
#SBATCH -p ${QUEUE_NAME}
#SBATCH -N ${NODES}
#SBATCH -n ${NODES}
#SBATCH -t ${MINUTES}
${SBATCH_ALLOCATION_LINE}

${MULTI_PILOT_BIN} ${PILOT_BIN} ${PILOT_DIR}
" >> ${PILOT_DIR}/stampede2.slurm

#
# Submit the SLURM job.
#
echo "Submitting SLURM job..."
SBATCH_LOG=${PILOT_DIR}/sbatch.log
sbatch ${PILOT_DIR}/stampede2.slurm &> ${SBATCH_LOG}
SBATCH_ERROR=$?
if [[ $SBATCH_ERROR != 0 ]]; then
    echo "Failed to submit job to SLURM (${SBATCH_ERROR}), aborting."
    cat ${SBATCH_LOG}
    exit 6
fi
JOB_ID=`cat ${SBATCH_LOG} | awk '/^Submitted batch job/{print $4}'`
echo "${CONTROL_PREFIX} JOB_ID ${JOB_ID}"
echo "..done."

# Reset the EXIT trap so that we don't delete the temporary directory
# that the SLURM job needs.  (We pass it the temporary directory so that
# it can clean up after itself.)
trap EXIT
exit 0
