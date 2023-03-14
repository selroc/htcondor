#!/bin/bash
# Exit on any error
set -e

# HTConder version
HTCONDOR_VERSION=$1
echo "HTCondor version is $HTCONDOR_VERSION"

# shellcheck disable=SC2206 # don't have to worry abour word splitting
AVERSION=(${HTCONDOR_VERSION//./ })
MAJOR_VER=${AVERSION[0]}
MINOR_VER=${AVERSION[1]}
if [ "$MINOR_VER" -eq 0 ]; then
    REPO_VERSION=$MAJOR_VER.0
else
    REPO_VERSION=$MAJOR_VER.x
fi

# Platform variables
VERSION_CODENAME='none'
. /etc/os-release
echo "Building on $NAME $VERSION"
VERSION_ID=${VERSION_ID%%.*}
ARCH=$(arch)
echo "ID=$ID VERSION_ID=$VERSION_ID VERSION_CODENAME=$VERSION_CODENAME ARCH=$ARCH"

if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    SUDO_GROUP='sudo'
else
    SUDO_GROUP='wheel'
fi

if [ $ID = 'amzn' ]; then
    yum install -y shadow-utils
fi

# Add users that might be used in CHTC
# The HTCondor that runs inside the container needs to have the user defined
for i in {1..161}; do
    uid=$((i+5000));
    useradd --uid  $uid --gid $SUDO_GROUP --shell /bin/bash --create-home slot$i;
done

for i in {1..161}; do
    uid=$((i+5299));
    useradd --uid  $uid --gid $SUDO_GROUP --shell /bin/bash --create-home slot1_$i;
done

useradd --uid  6004 --gid $SUDO_GROUP --shell /bin/bash --create-home condorauto
useradd --uid 22537 --gid $SUDO_GROUP --shell /bin/bash --create-home bbockelm
useradd --uid 20343 --gid $SUDO_GROUP --shell /bin/bash --create-home blin
useradd --uid 24200 --gid $SUDO_GROUP --shell /bin/bash --create-home cabollig
useradd --uid 20003 --gid $SUDO_GROUP --shell /bin/bash --create-home cat
useradd --uid 20342 --gid $SUDO_GROUP --shell /bin/bash --create-home edquist
useradd --uid 20006 --gid $SUDO_GROUP --shell /bin/bash --create-home gthain
useradd --uid 20839 --gid $SUDO_GROUP --shell /bin/bash --create-home iaross
useradd --uid 21356 --gid $SUDO_GROUP --shell /bin/bash --create-home jcpatton
useradd --uid 20007 --gid $SUDO_GROUP --shell /bin/bash --create-home jfrey
useradd --uid 20018 --gid $SUDO_GROUP --shell /bin/bash --create-home johnkn
useradd --uid 20020 --gid $SUDO_GROUP --shell /bin/bash --create-home matyas
useradd --uid 20013 --gid $SUDO_GROUP --shell /bin/bash --create-home tannenba
useradd --uid 20345 --gid $SUDO_GROUP --shell /bin/bash --create-home tim
useradd --uid 20015 --gid $SUDO_GROUP --shell /bin/bash --create-home tlmiller

# Provide a condor_config.generic
mkdir -p /usr/local/condor/etc/examples
echo 'use SECURITY : HOST_BASED' > /usr/local/condor/etc/examples/condor_config.generic

if [ $ID = debian ] || [ $ID = 'ubuntu' ]; then
    apt update
    export DEBIAN_FRONTEND='noninteractive'
    INSTALL='apt install --yes'
elif [ $ID = 'amzn' ] || [ $ID = 'centos' ]; then
    INSTALL='yum install --assumeyes'
elif [ $ID = 'almalinux' ] || [ $ID = 'fedora' ]; then
    INSTALL='dnf install --assumeyes'
    $INSTALL 'dnf-command(config-manager)'
fi

if [ $ID = 'almalinux' ] || [ $ID = 'centos' ]; then
    $INSTALL epel-release
    if [ $VERSION_ID -eq 7 ]; then
        $INSTALL centos-release-scl
    elif [ $VERSION_ID -eq 8 ]; then
        dnf config-manager --set-enabled powertools
    elif [ $VERSION_ID -eq 9 ]; then
        dnf config-manager --set-enabled crb
    fi
fi

if [ $ID = 'amzn' ]; then
    $INSTALL http://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm \
             http://mirror.centos.org/centos/7/extras/x86_64/Packages/centos-release-scl-2-3.el7.centos.noarch.rpm \
             http://mirror.centos.org/centos/7/extras/x86_64/Packages/centos-release-scl-rh-2-3.el7.centos.noarch.rpm \
             http://mirror.centos.org/centos/7/os/x86_64/Packages/libgfortran5-8.3.1-2.1.1.el7.x86_64.rpm
fi

if [ $ID = 'amzn' ]; then
    $INSTALL "https://research.cs.wisc.edu/htcondor/repo/$REPO_VERSION/htcondor-release-current.amzn$VERSION_ID.noarch.rpm"
fi

if [ $ID = 'almalinux' ] || [ $ID = 'centos' ]; then
    $INSTALL "https://research.cs.wisc.edu/htcondor/repo/$REPO_VERSION/htcondor-release-current.el$VERSION_ID.noarch.rpm"
fi

if [ $ID = 'fedora' ]; then
    $INSTALL "https://research.cs.wisc.edu/htcondor/repo/$REPO_VERSION/htcondor-release-current.f$VERSION_ID.noarch.rpm"
fi

# Setup Debian based repositories
if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    $INSTALL apt-transport-https curl gnupg
    curl -fsSL "https://research.cs.wisc.edu/htcondor/repo/keys/HTCondor-${REPO_VERSION}-Key" | apt-key add -
    curl -fsSL "https://research.cs.wisc.edu/htcondor/repo/$ID/htcondor-${REPO_VERSION}-${VERSION_CODENAME}.list" -o /etc/apt/sources.list.d/htcondor.list
    apt update
fi

# Use the testing repositories for unreleased software
if [ $VERSION_CODENAME = 'bionic' ]; then
    cp -p /etc/apt/sources.list.d/htcondor.list /etc/apt/sources.list.d/htcondor-test.list
    sed -i s+repo/+repo-test/+ /etc/apt/sources.list.d/htcondor-test.list
    apt update
fi
if [ $ID = 'future' ]; then
    cp -p /etc/yum.repos.d/htcondor.repo /etc/yum.repos.d/htcondor-test.repo
    sed -i s+repo/+repo-test/+ /etc/yum.repos.d/htcondor-test.repo
    sed -i s/\\[htcondor/[htcondor-test/ /etc/yum.repos.d/htcondor-test.repo
    # ] ] Help out vim syntax highlighting
fi

# Install the build dependencies
if [ $ID = 'almalinux' ] || [ $ID = 'amzn' ] || [ $ID = 'centos' ] || [ $ID = 'fedora' ]; then
    $INSTALL make rpm-build yum-utils
    yum-builddep -y /tmp/rpm/condor.spec
fi

# Need newer cmake on bionic
if [ $VERSION_CODENAME = 'bionic' ]; then
    curl -dsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | apt-key add -
    echo 'deb https://apt.kitware.com/ubuntu/ bionic main' > /etc/apt/sources.list.d/cmake.list
    apt update
fi

if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    $INSTALL build-essential devscripts equivs gpp
    (cd /tmp/debian; ./prepare-build-files.sh)
    mk-build-deps --install --tool='apt-get -o Debug::pkgProblemResolver=yes --no-install-recommends --yes' /tmp/debian/control
fi

if [ $VERSION_CODENAME = 'bionic' ]; then
    # Need to upgrade compiler on this old platform
    $INSTALL gcc-8 g++-8
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 800 --slave /usr/bin/g++ g++ /usr/bin/g++-8
fi

# Add useful debugging tools
$INSTALL gdb git less nano patchelf python3-pip strace sudo vim
if [ $ID = 'almalinux' ] || [ $ID = 'amzn' ] || [ $ID = 'centos' ] || [ $ID = 'fedora' ]; then
    $INSTALL iputils rpmlint
fi
if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    $INSTALL lintian net-tools
fi

# Container users can sudo
echo "%$SUDO_GROUP ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/$SUDO_GROUP

# Install HTCondor to build and test BaTLab style
if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    $INSTALL htcondor libnss-myhostname openssh-server
    # Ensure that gethostbyaddr() returns our hostname
    sed -i -e 's/^hosts:.*/& myhostname/' /etc/nsswitch.conf
fi

if [ $ID = 'almalinux' ] || [ $ID = 'amzn' ] || [ $ID = 'centos' ] || [ $ID = 'fedora' ]; then
    $INSTALL condor java procps-ng openssh-clients openssh-server
    if [ $ID != 'amzn' ]; then
        $INSTALL apptainer
    fi
    $INSTALL 'perl(Archive::Tar)' 'perl(Data::Dumper)' 'perl(Digest::MD5)' 'perl(Digest::SHA)' 'perl(English)' 'perl(Env)' 'perl(File::Copy)' 'perl(FindBin)' 'perl(Net::Domain)' 'perl(Sys::Hostname)' 'perl(Time::HiRes)' 'perl(XML::Parser)'
fi

# Include packages for tarball in the image.
externals_dir="/usr/local/condor/externals/$REPO_VERSION"
mkdir -p "$externals_dir"
if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    (cd "$externals_dir";
        apt download condor-stash-plugin libcgroup1 libgomp1 libmunge2 libpcre2-8-0 libscitokens0 libvomsapi1v5)
    if [ $VERSION_CODENAME = 'bullseye' ]; then
        (cd "$externals_dir"; apt download libboost-python1.74.0)
    elif [ $VERSION_CODENAME = 'bookworm' ]; then
        (cd "$externals_dir"; apt download libboost-python1.74.0)
    elif [ $VERSION_CODENAME = 'bionic' ]; then
        (cd "$externals_dir"; apt download libboost-python1.65.1)
    elif [ $VERSION_CODENAME = 'focal' ]; then
        (cd "$externals_dir"; apt download libboost-python1.71.0)
    elif [ $VERSION_CODENAME = 'jammy' ]; then
        (cd "$externals_dir"; apt download libboost-python1.74.0)
    else
        echo "Unknown codename: $VERSION_CODENAME"
        exit 1
    fi
fi
if [ $ID = 'almalinux' ] || [ $ID = 'amzn' ] || [ $ID = 'centos' ] || [ $ID = 'fedora' ]; then
    yumdownloader --downloadonly --destdir="$externals_dir" \
        condor-stash-plugin libgomp munge-libs pcre2 scitokens-cpp voms
    if [ $ID = 'centos' ] && [ $VERSION_ID -eq 7 ]; then
        yumdownloader --downloadonly --destdir="$externals_dir" \
            boost169-python3 python36-chardet python36-idna python36-pysocks python36-requests python36-six python36-urllib3
    else
        yumdownloader --downloadonly --destdir="$externals_dir" boost-python3
    fi
    if [ $VERSION_ID -lt 9 ]; then
        yumdownloader --downloadonly --destdir="$externals_dir" libcgroup
    fi
    # Remove 32-bit x86 packages if any
    rm -f "$externals_dir"/*.i686.rpm
fi

# Clean up package caches
if [ $ID = 'amzn' ] || [ $ID = 'centos' ]; then
    yum clean all
    rm -rf /var/cache/yum/*
fi
if [ $ID = 'almalinux' ] || [ $ID = 'fedora' ]; then
    dnf clean all
    rm -rf /var/cache/yum/*
fi
if [ $ID = 'debian' ] || [ $ID = 'ubuntu' ]; then
    apt -y autoremove
    apt -y clean
fi

# Install pytest for BaTLab testing
pip3 install pytest pytest-httpserver

if [ $VERSION_CODENAME = 'bullseye' ] || [ $VERSION_CODENAME = 'focal' ]; then
    # Pip installs a updated version of markupsafe that is incompatiable
    # with sphinx on this platform. Downgrade markupsafe and hope for the best
    pip3 install markupsafe==2.0.1
fi

rm -rf /tmp/*
exit 0
