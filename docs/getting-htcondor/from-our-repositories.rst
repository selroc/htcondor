.. _from_our_repos:

Linux (from our repositories)
=============================

If you're not already familiar with HTCondor, we recommend you follow our
:ref:`instructions <admin_install_linux>` for your first installation.

If you're looking to automate the installation of HTCondor using your existing
toolchain, the latest information is embedded in the output of the script run
as part of the :ref:`instructions <admin_install_linux>`.  This script can
be run as a normal user (or ``nobody``), so we recommend this approach.

Otherwise, this page contains information about the RPM and deb
repositories we offer.  These repositories will almost always have more
recent releases than the distributions.

RPM-based Distributions
-----------------------

We support several RPM-based platforms: Enterprise Linux 7, including Red Hat, CentOS, and Scientific Linux;
Enterprise Linux 8, including Red Hat, CentOS Stream, Alma Linux, and Rocky Linux; Enterprise Linux 9,
including Red Hat, CentOS Stream, Alma Linux, and Rocky Linux.  Binaries are available
for x86-64 for all these platforms.  For Enterprise Linux 8,
HTCondor also supports ARM ("aarch64") and Power ("ppc64le").
For Enterprise Linux 9, HTCondor also supports ARM ("aarch64").

Repository packages are available for each platform:

* `Enterprise Linux 7 <https://research.cs.wisc.edu/htcondor/repo/10.x/htcondor-release-current.el7.noarch.rpm>`_
* `Enterprise Linux 8 <https://research.cs.wisc.edu/htcondor/repo/10.x/htcondor-release-current.el8.noarch.rpm>`_
* `Enterprise Linux 9 <https://research.cs.wisc.edu/htcondor/repo/10.x/htcondor-release-current.el9.noarch.rpm>`_

The HTCondor packages on these platforms depend on the corresponding
version of `EPEL <https://fedoraproject.org/wiki/EPEL>`_.

Additionally, the following repositories are required for specific platforms:

* On RedHat 7, ``rhel-*-optional-rpms``, ``rhel-*-extras-rpms``, and
  ``rhel-ha-for-rhel-*-server-rpms``.
* On RedHat 8, ``codeready-builder-for-rhel-8-${ARCH}-rpms``.
* On CentOS 8, ``powertools`` (or ``PowerTools``).
* On RedHat 9, ``crb``.

deb-based Distributions
-----------------------

We support three deb-based platforms: Debian 11 (Bullseye); and
Ubuntu 20.04 (Focal Fossa) and 22.04 (Jammy Jellyfish).
Binaries are only available for x86-64.
These repositories also include the source packages.

Debian 11
#########

Add our `Debian signing key <https://research.cs.wisc.edu/htcondor/repo/keys/HTCondor-10.x-Key>`_
with ``apt-key add`` before adding the repositories below.

* Debian 11: ``deb [arch=amd64] http://research.cs.wisc.edu/htcondor/repo/debian/10.x bullseye main``

Ubuntu 20.04, and 22.04
#######################

Add our `Ubuntu signing key <https://research.cs.wisc.edu/htcondor/repo/keys/HTCondor-10.x-Key>`_
with ``apt-key add`` before adding the repositories below.

* Ubuntu 20.04: ``deb [arch=amd64] http://research.cs.wisc.edu/htcondor/repo/ubuntu/10.x focal main``
* Ubuntu 22.04: ``deb [arch=amd64] http://research.cs.wisc.edu/htcondor/repo/ubuntu/10.x jammy main``
