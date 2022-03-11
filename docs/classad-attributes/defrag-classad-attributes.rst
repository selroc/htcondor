Defrag ClassAd Attributes
=========================

:index:`Defrag attributes<single: Defrag attributes; ClassAd>`

:classad-attribute:`AvgDrainingBadput`
    Fraction of time CPUs in the pool have spent on jobs that were
    killed during draining of the machine. This is calculated in each
    polling interval by looking at ``TotalMachineDrainingBadput``.
    Therefore, it treats evictions of jobs that do and do not produce
    checkpoints the same. When the *condor_startd* restarts, its
    counters start over from 0, so the average is only over the time
    since the daemons have been alive.

:classad-attribute:`AvgDrainingUnclaimedTime`
    Fraction of time CPUs in the pool have spent unclaimed by a user
    during draining of the machine. This is calculated in each polling
    interval by looking at ``TotalMachineDrainingUnclaimedTime``. When
    the *condor_startd* restarts, its counters start over from 0, so
    the average is only over the time since the daemons have been alive.

:classad-attribute:`DaemonStartTime`
    The time that this daemon was started, represented as the number of
    seconds elapsed since the Unix epoch (00:00:00 UTC, Jan 1, 1970).

:classad-attribute:`DaemonLastReconfigTime`
    The time that this daemon was configured, represented as the number
    of seconds elapsed since the Unix epoch (00:00:00 UTC, Jan 1, 1970).

:classad-attribute:`DrainedMachines`
    A count of the number of fully drained machines which have arrived
    during the run time of this *condor_defrag* daemon.

:classad-attribute:`DrainFailures`
    Total count of failed attempts to initiate draining during the
    lifetime of this *condor_defrag* daemon.

:classad-attribute:`DrainSuccesses`
    Total count of successful attempts to initiate draining during the
    lifetime of this *condor_defrag* daemon.

:classad-attribute:`Machine`
    A string with the machine's fully qualified host name.

:classad-attribute:`MachinesDraining`
    Number of machines that were observed to be draining in the last
    polling interval.

:classad-attribute:`MachinesDrainingPeak`
    Largest number of machines that were ever observed to be draining.

:classad-attribute:`MeanDrainedArrived`
    The mean time in seconds between arrivals of fully drained machines.

:classad-attribute:`MonitorSelfAge`
    The number of seconds that this daemon has been running.

:classad-attribute:`MonitorSelfCPUUsage`
    The fraction of recent CPU time utilized by this daemon.

:classad-attribute:`MonitorSelfImageSize`
    The amount of virtual memory consumed by this daemon in KiB.

:classad-attribute:`MonitorSelfRegisteredSocketCount`
    The current number of sockets registered by this daemon.

:classad-attribute:`MonitorSelfResidentSetSize`
    The amount of resident memory used by this daemon in KiB.

:classad-attribute:`MonitorSelfSecuritySessions`
    The number of open (cached) security sessions for this daemon.

:classad-attribute:`MonitorSelfTime`
    The time, represented as the number of seconds elapsed since the
    Unix epoch (00:00:00 UTC, Jan 1, 1970), at which this daemon last
    checked and set the attributes with names that begin with the string
    ``MonitorSelf``.

:classad-attribute:`MyAddress`
    String with the IP and port address of the *condor_defrag* daemon
    which is publishing this ClassAd.

:classad-attribute:`MyCurrentTime`
    The time, represented as the number of seconds elapsed since the
    Unix epoch (00:00:00 UTC, Jan 1, 1970), at which the
    *condor_defrag* daemon last sent a ClassAd update to the
    *condor_collector*.

:classad-attribute:`Name`
    The name of this daemon; typically the same value as the ``Machine``
    attribute, but could be customized by the site administrator via the
    configuration variable ``DEFRAG_NAME`` :index:`DEFRAG_NAME`.

:classad-attribute:`RecentCancelsList`
    A ClassAd list of ClassAds describing the last ten cancel commands sent
    by this daemon.  Attributes include ``when``, as the number of seconds
    since the Unix epoch; and ``who``, the ``Name`` of the slot being drained.

:classad-attribute:`RecentDrainFailures`
    Count of failed attempts to initiate draining during the past
    ``RecentStatsLifetime`` seconds.

:classad-attribute:`RecentDrainSuccesses`
    Count of successful attempts to initiate draining during the past
    ``RecentStatsLifetime`` seconds.

:classad-attribute:`RecentDrainsList`
    A ClassAd list of ClassAds describing the last ten drain commands sent
    by this daemon.  Attributes include ``when``, as the number of seconds
    since the Unix epoch; ``who``, the ``Name`` of the slot being drained;
    and ``what``, one of the three strings ``graceful``, ``quick``, or
    ``fast``.

:classad-attribute:`RecentStatsLifetime`
    A Statistics attribute defining the time in seconds over which
    statistics values have been collected for attributes with names that
    begin with ``Recent``.

:classad-attribute:`UpdateSequenceNumber`
    An integer, starting at zero, and incremented with each ClassAd
    update sent to the *condor_collector*. The *condor_collector* uses
    this value to sequence the updates it receives.

:classad-attribute:`WholeMachines`
    Number of machines that were observed to be defragmented in the last
    polling interval.

:classad-attribute:`WholeMachinesPeak`
    Largest number of machines that were ever observed to be
    simultaneously defragmented.
