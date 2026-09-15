#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

OUTDIR=$(realpath "${OUTDIR}")
if [ -z "${OUTDIR}" ] || [ "${OUTDIR}" = "/" ]; then
    echo "Refusing to operate with OUTDIR='${OUTDIR}'" 1>&2
    exit 1
fi

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
fi

echo "Adding the Image in outdir"
cp -L "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p "${OUTDIR}/rootfs"
cd "${OUTDIR}/rootfs"
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr/bin usr/lib usr/sbin var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make defconfig
else
    cd busybox
fi

# TODO: Make and install busybox
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX="${OUTDIR}/rootfs" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

cd "${OUTDIR}/rootfs"

echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

INTERP=$(${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter" | sed -n 's/.*: \(.*\)\]/\1/p')
if [ -z "${INTERP}" ]; then
    echo "Could not determine program interpreter from busybox" 1>&2
    exit 1
fi
mkdir -p lib lib64
cp -L "${SYSROOT}${INTERP}" lib/

while read -r LIB; do
    LIBPATH=$(find "${SYSROOT}" -name "${LIB}" | head -n1)
    if [ -z "${LIBPATH}" ]; then
        echo "Could not locate ${LIB} under ${SYSROOT}" 1>&2
        exit 1
    fi
    cp -L "${LIBPATH}" lib64/
done < <(${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library" | sed -n 's/.*: \[\(.*\)\]/\1/p')

# TODO: Make device nodes
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1

# TODO: Clean and build the writer utility
cd "${FINDER_APP_DIR}"
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
mkdir -p "${OUTDIR}/rootfs/home"
cp -p "${FINDER_APP_DIR}/writer" "${OUTDIR}/rootfs/home/"
cp -p "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp -p "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/"
cp -p "${FINDER_APP_DIR}/writer.sh" "${OUTDIR}/rootfs/home/"
cp -p "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/"

mkdir -p "${OUTDIR}/rootfs/home/conf" "${OUTDIR}/rootfs/conf"
cp -p "${FINDER_APP_DIR}/conf/"*.txt "${OUTDIR}/rootfs/home/conf/"
cp -p "${FINDER_APP_DIR}/conf/"*.txt "${OUTDIR}/rootfs/conf/"

# /bin/bash shim: finder.sh/writer.sh have #!/bin/bash shebangs, but a
# from-scratch BusyBox rootfs has no bash applet (verified empirically
# below). A bin/bash -> busybox symlink would fail: busybox dispatches on
# basename(argv[0]) and ships no "bash" applet. Provide a tiny /bin/sh
# wrapper instead, unless the rootfs already has a real bash applet.
if [ ! -e "${OUTDIR}/rootfs/bin/bash" ]; then
    cat > "${OUTDIR}/rootfs/bin/bash" <<'EOF'
#!/bin/sh
exec /bin/sh "$@"
EOF
    chmod 755 "${OUTDIR}/rootfs/bin/bash"
fi

# TODO: Chown the root directory
sudo chown -R root:root "${OUTDIR}/rootfs"

# TODO: Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root > "${OUTDIR}/initramfs.cpio"
gzip -f "${OUTDIR}/initramfs.cpio"
