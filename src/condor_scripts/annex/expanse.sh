#!/bin/bash

CONTROL_PREFIX="=-.-="
echo "${CONTROL_PREFIX} PID $$"

function usage() {
    echo "Usage: ${0} \\"
    echo "       JOB_NAME QUEUE_NAME COLLECTOR TOKEN_FILE LIFETIME PILOT_BIN \\"
    echo "       OWNERS NODES MULTI_PILOT_BIN ALLOCATION REQUEST_ID PASSWORD_FILE \\"
    echo "       [CPUS] [MEM_MB]"
    echo "where OWNERS is a comma-separated list.  Omit CPUS and MEM_MB to get"
    echo "whole-node jobs.  NODES is ignored on non-whole-node jobs."
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

CPUS=${13}
if [[ $CPUS == "None" ]]; then
    CPUS=""
fi

MEM_MB=${14}
if [[ $MEM_MB == "None" ]]; then
    MEM_MB=""
fi

BIRTH=`date +%s`
# echo "Starting script at `date`..."

#
# Download and configure the pilot on the head node before running it
# on the execute node(s).
#
# The following variables are constants.
#


# The binaries must be a tarball named condor-*, and unpacking that tarball
# must create a directory which also matches condor-*.
WELL_KNOWN_LOCATION_FOR_BINARIES=https://research.cs.wisc.edu/htcondor/tarball/current/9.5.4/update/condor-9.5.4-20220207-x86_64_Rocky8-stripped.tar.gz

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

# echo "Creating temporary directory for pilot..."
echo "Step 1 of 8..."
SCRATCH=${SCRATCH:-/expanse/lustre/scratch/$USER/temp_project}
mkdir -p "$SCRATCH"
PILOT_DIR=`/usr/bin/mktemp --directory --tmpdir=${SCRATCH} pilot.XXXXXXXX 2>&1`
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

# echo "Downloading configuration..."
echo "Step 2 of 8..."
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
# echo "Downloading binaries..."
echo "Step 3 of 8..."
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
# echo "Unpacking binaries..."
echo "Step 4 of 8..."
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

# echo "Making a personal condor..."
echo "Step 5 of 8..."
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
# `module load singularitypro` before starting the master.
# Using a wrapper with bash -l (to load the rc files; without -l, PATH
# wouldn't be set).  bash -l is quiet on Expanse (unlike Stampede2).
#
echo '#!/bin/bash -l
export USER=`/usr/bin/id -un`
module load singularitypro
exec singularity "$@"
' > ${PILOT_DIR}/singularity.sh
chmod 755 ${PILOT_DIR}/singularity.sh

# It may have take some time to get everything installed, so to make sure
# we get our full clean-up time, subtract off how long we've been running
# already.
YOUTH=$((`date +%s` - ${BIRTH}))
REMAINING_LIFETIME=$(((${LIFETIME} - ${YOUTH}) - ${CLEAN_UP_TIME}))


WHOLE_NODE=1
CONDOR_CPUS_LINE=""
if [[ -n $CPUS && $CPUS -gt 0 ]]; then
    CONDOR_CPUS_LINE="NUM_CPUS = ${CPUS}"
    WHOLE_NODE=""
fi

CONDOR_MEMORY_LINE=""
if [[ -n $MEM_MB && $MEM_MB -gt 0 ]]; then
    CONDOR_MEMORY_LINE="MEMORY = ${MEM_MB}"
    WHOLE_NODE=""
fi

# echo "Converting to a pilot..."
echo "Step 6 of 8..."
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

# This is made available via 'module load singularitypro', but the
# starter ignores PATH, so wrap it up.
SINGULARITY = ${PILOT_DIR}/singularity.sh

${CONDOR_CPUS_LINE}
${CONDOR_MEMORY_LINE}

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

# echo "Unpacking configuration..."
echo "Step 7 of 8..."
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

if [[ $WHOLE_NODE ]]; then
    # Whole node jobs request the same number of tasks per node as tasks total.
    # They make no specific requests about CPUs, memory, etc., since the SLURM
    # partition should already determine that.
    SBATCH_RESOURCES_LINES="\
#SBATCH --nodes=${NODES}
#SBATCH --ntasks=${NODES}
"
else
    # Jobs on shared (non-whole-node) SLURM partitions can't be multi-node on
    # Expanse.  Request one job, and specify the resources that should be
    # allocated to the job.

    # XXX Should I reject NODES > 1?
    # FIXME: I'm OK with ignoring it, but the FE should check..
    SBATCH_RESOURCES_LINES="\
#SBATCH --ntasks=1
#SBATCH --nodes=1
"
    if [[ $CPUS ]]; then
        SBATCH_RESOURCES_LINES="\
${SBATCH_RESOURCES_LINES}
#SBATCH --cpus-per-task=${CPUS}
"
    fi
    if [[ $MEM_MB ]]; then
        SBATCH_RESOURCES_LINES="\
${SBATCH_RESOURCES_LINES}
#SBATCH --mem=${MEM_MB}M
"
    fi
fi


if [[ -n $ALLOCATION ]]; then
    SBATCH_ALLOCATION_LINE="#SBATCH -A ${ALLOCATION}"
fi

echo '#!/bin/bash' > ${PILOT_DIR}/expanse.slurm
echo "
#SBATCH -J ${JOB_NAME}
#SBATCH -o ${PILOT_DIR}/%j.out
#SBATCH -e ${PILOT_DIR}/%j.err
#SBATCH -p ${QUEUE_NAME}
${SBATCH_RESOURCES_LINES}
#SBATCH -t ${MINUTES}
${SBATCH_ALLOCATION_LINE}

${MULTI_PILOT_BIN} ${PILOT_BIN} ${PILOT_DIR}
" >> ${PILOT_DIR}/expanse.slurm

#
# Submit the SLURM job.
#
# echo "Submitting SLURM job..."
echo "Step 8 of 8..."
SBATCH_LOG=${PILOT_DIR}/sbatch.log
sbatch ${PILOT_DIR}/expanse.slurm &> ${SBATCH_LOG}
SBATCH_ERROR=$?
if [[ $SBATCH_ERROR != 0 ]]; then
    echo "Failed to submit job to SLURM (${SBATCH_ERROR}), aborting."
    cat ${SBATCH_LOG}
    exit 6
fi
JOB_ID=`cat ${SBATCH_LOG} | awk '/^Submitted batch job/{print $4}'`
echo "${CONTROL_PREFIX} JOB_ID ${JOB_ID}"
echo "... done."

# Reset the EXIT trap so that we don't delete the temporary directory
# that the SLURM job needs.  (We pass it the temporary directory so that
# it can clean up after itself.)
trap EXIT
exit 0
