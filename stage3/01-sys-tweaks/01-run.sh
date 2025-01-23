#!/bin/bash

SUB_STAGE_DIR=${PWD}

#
# Fixup absolute links to relative in /usr/lib and libs in /etc/alternatives
#
pushd "${ROOTFS_DIR}"
find ./usr/lib -lname '/*' | \
  while read l
  do
    echo ln -sf $(echo $(echo $l | sed 's|/[^/]*|/..|g')$(readlink $l) | sed 's/.....//') $l
  done | sh
find ./usr/include -lname '/*' | \
  while read l
  do
    echo ln -sf $(echo $(echo $l | sed 's|/[^/]*|/..|g')$(readlink $l) | sed 's/.....//') $l
  done | sh
find ./etc/alternatives -lname '/*.so.*' | \
  while read l
  do
    echo ln -sf $(echo $(echo $l | sed 's|/[^/]*|/..|g')$(readlink $l) | sed 's/.....//') $l
  done | sh
find ./etc/alternatives -lname '/*/aarch64-linux-gnu/*' | \
  while read l
  do
    echo ln -sf $(echo $(echo $l | sed 's|/[^/]*|/..|g')$(readlink $l) | sed 's/.....//') $l
  done | sh
popd

#
# Add symbolic link for cblas.h to /usr/include (required by OpenCV)
#
#ln -sf aarch64-linux-gnu/cblas.h "${ROOTFS_DIR}/usr/include/cblas.h"

#
# Download sources
#
DOWNLOAD_DIR=${STAGE_WORK_DIR}/download
mkdir -p ${DOWNLOAD_DIR}
pushd ${DOWNLOAD_DIR}

# opencv sources
wget -nc -nv \
    https://github.com/opencv/opencv/archive/4.10.0.tar.gz
wget -nc -nv -O contrib-4.10.0.tar.gz \
    https://github.com/opencv/opencv_contrib/archive/4.10.0.tar.gz

# allwpilib
wget -nc -nv -O allwpilib.tar.gz \
    https://github.com/wpilibsuite/allwpilib/archive/v2025.2.1.tar.gz

## robotpy-build
#wget -nc -nv -O robotpy-build.tar.gz \
#    https://github.com/robotpy/robotpy-build/archive/2023.0.0.tar.gz
#
## pybind11
#wget -nc -nv -O pybind11.tar.gz \
#    https://github.com/pybind/pybind11/archive/8ece7d641ca6ce316e59fec6744b8517073bbe32.tar.gz
#
## robotpy-wpiutil
#wget -nc -nv -O robotpy-wpiutil.tar.gz \
#    https://github.com/robotpy/robotpy-wpiutil/archive/2023.1.1.0.tar.gz
#
## robotpy-wpinet
#wget -nc -nv -O robotpy-wpinet.tar.gz \
#    https://github.com/robotpy/robotpy-wpinet/archive/2023.1.1.0.tar.gz
#
## pyntcore
#wget -nc -nv -O pyntcore.tar.gz \
#    https://github.com/robotpy/pyntcore/archive/2023.1.1.0.tar.gz
#
## robotpy-cscore
#wget -nc -nv -O robotpy-cscore.tar.gz \
#    https://github.com/robotpy/robotpy-cscore/archive/2023.1.1.0.tar.gz

# pixy2
wget -nc -nv -O pixy2.tar.gz \
    https://github.com/charmedlabs/pixy2/archive/2adc6caba774a3056448d0feb0c6b89855a392f4.tar.gz

popd

#
# Extract and patch sources
#
EXTRACT_DIR=${ROOTFS_DIR}/usr/src
install -v -d ${EXTRACT_DIR}
pushd ${EXTRACT_DIR}

# opencv
rm -rf opencv-4.10.0
tar xzf "${DOWNLOAD_DIR}/4.10.0.tar.gz"
tar xzf "${DOWNLOAD_DIR}/contrib-4.10.0.tar.gz"
pushd opencv-4.10.0
sed -i -e 's/javac sourcepath/javac target="1.8" source="1.8" sourcepath/' modules/java/jar/build.xml.in
# disable extraneous data warnings; these are common with USB cameras
sed -i -e '/JWRN_EXTRANEOUS_DATA/d' 3rdparty/libjpeg/jdmarker.c
sed -i -e '/JWRN_EXTRANEOUS_DATA/d' 3rdparty/libjpeg-turbo/src/jdmarker.c
popd

# allwpilib
tar xzf "${DOWNLOAD_DIR}/allwpilib.tar.gz"
rm -rf allwpilib
mv allwpilib-* allwpilib
pushd allwpilib
popd

# pixy2
tar xzf "${DOWNLOAD_DIR}/pixy2.tar.gz"
rm -rf pixy2
mv pixy2-* pixy2
rm -rf pixy2/releases
sed -i -e 's/g++/aarch64-linux-gnu-g++/' pixy2/src/host/libpixyusb2/src/Makefile
sed -i -e 's/^python/python3/;s/_pixy.so/_pixy.*.so/' pixy2/scripts/build_python_demos.sh
sed -i -e 's/print/#print/' pixy2/src/host/libpixyusb2_examples/python_demos/setup.py

popd

#
# Build
#

# get number of CPU cores
NCPU=`grep -c 'cpu[0-9]' /proc/stat`

export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR=${ROOTFS_DIR}/usr/lib/aarch64-linux-gnu/pkgconfig:${ROOTFS_DIR}/usr/lib/pkgconfig:${ROOTFS_DIR}/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=${ROOTFS_DIR}

pushd ${STAGE_WORK_DIR}
#
# Build OpenCV
#
build_opencv () {
    rm -rf $1
    mkdir -p $1
    pushd $1
    cmake "${EXTRACT_DIR}/opencv-4.10.0" \
	-G Ninja \
	-DWITH_FFMPEG=OFF \
        -DBUILD_JPEG=ON \
        -DBUILD_TIFF=ON \
        -DBUILD_TESTS=OFF \
        -DPython_ADDITIONAL_VERSIONS=3.11 \
        -DBUILD_JAVA=$3 \
        -DENABLE_CXX11=ON \
        -DBUILD_SHARED_LIBS=$3 \
        -DCMAKE_BUILD_TYPE=$2 \
        -DCMAKE_DEBUG_POSTFIX=d \
        -DCMAKE_TOOLCHAIN_FILE=${ROOTFS_DIR}/usr/src/opencv-4.10.0/platforms/linux/aarch64-gnu.toolchain.cmake \
        -DARM_LINUX_SYSROOT=${ROOTFS_DIR} \
        -DCMAKE_SYSROOT=${ROOTFS_DIR} \
        -DENABLE_NEON=ON \
        -DWITH_TBB=$3 \
        -DBUILD_opencv_python3=$3 \
        -DPYTHON3_INCLUDE_PATH=${ROOTFS_DIR}/usr/include/python3.11 \
        -DPYTHON3_NUMPY_INCLUDE_DIRS=${ROOTFS_DIR}/usr/include/python3.11/numpy \
        -DOPENCV_EXTRA_FLAGS_DEBUG=-Og \
	-DOPENCV_GENERATE_PKGCONFIG=ON \
        -DCMAKE_MODULE_PATH=${SUB_STAGE_DIR}/files \
        -DCMAKE_INSTALL_PREFIX=/usr/local/frc$4 \
	-DOPENCV_EXTRA_MODULES_PATH=${EXTRACT_DIR}/opencv_contrib-4.10.0/modules/aruco \
        || exit 1
    ninja || exit 1
    env DESTDIR=${ROOTFS_DIR} ninja install || exit 1
    popd
}

build_opencv build/opencv-build-debug Debug ON "" || exit 1
build_opencv build/opencv-build Release ON "" || exit 1
build_opencv build/opencv-static Release OFF "-static" || exit 1

# fix up java install
cp -p ${ROOTFS_DIR}/usr/local/frc/share/java/opencv4/libopencv_java480*.so "${ROOTFS_DIR}/usr/local/frc/lib/"
mkdir -p "${ROOTFS_DIR}/usr/local/frc/java"
cp -p "${ROOTFS_DIR}/usr/local/frc/share/java/opencv4/opencv-480.jar" "${ROOTFS_DIR}/usr/local/frc/java/"

# the opencv build names the python .so with the build platform name
# instead of the target platform, so rename it
pushd "${ROOTFS_DIR}/usr/local/frc/lib/python3.11/dist-packages/cv2/python-3.11"
mv cv2.cpython-311-*-gnu.so cv2.cpython-311-aarch64-linux-gnu.so
mv cv2d.cpython-311-*-gnu.so cv2d.cpython-311-aarch64-linux-gnu.so
popd

# link python package to dist-packages
ln -sf /usr/local/frc/lib/python3.11/dist-packages/cv2 "${ROOTFS_DIR}/usr/local/lib/python3.11/dist-packages/cv2"

#
# Build wpiutil, cscore, ntcore, cameraserver
# always use the release version of opencv jar/jni
#
build_wpilib () {
    rm -rf $1
    mkdir -p $1
    pushd $1
    cmake "${EXTRACT_DIR}/allwpilib" \
	-G Ninja \
        -DWITH_GUI=OFF \
        -DWITH_TESTS=OFF \
        -DWITH_SIMULATION_MODULES=OFF \
	-DWPILIB_TARGET_WARNINGS=-Wno-maybe-uninitialized \
        -DCMAKE_BUILD_TYPE=$2 \
        -DCMAKE_TOOLCHAIN_FILE=${ROOTFS_DIR}/usr/src/opencv-4.10.0/platforms/linux/aarch64-gnu.toolchain.cmake \
        -DARM_LINUX_SYSROOT=${ROOTFS_DIR} \
        -DCMAKE_SYSROOT=${ROOTFS_DIR} \
        -DCMAKE_MODULE_PATH=${SUB_STAGE_DIR}/files \
        -DOPENCV_JAR_FILE=`ls ${ROOTFS_DIR}/usr/local/frc/java/opencv-4100.jar` \
        -DOPENCV_JNI_FILE=`ls ${ROOTFS_DIR}/usr/local/frc/lib/libopencv_java4100.so` \
        -DOpenCV_DIR=${ROOTFS_DIR}/usr/local/frc/share/opencv4 \
        -DTHREADS_PTHREAD_ARG=-pthread \
        -DCMAKE_INSTALL_PREFIX=/usr/local/frc \
        || exit 1
    ninja || exit 1
    env DESTDIR=${ROOTFS_DIR} ninja install || exit 1
    popd
}

build_wpilib build/allwpilib-build-debug Debug || exit 1
build_wpilib build/allwpilib-build Release || exit 1

# static (for tools)
build_static_wpilib() {
    rm -rf $1
    mkdir -p $1
    pushd $1
    cmake "${EXTRACT_DIR}/allwpilib" \
	-G Ninja \
        -DWITH_GUI=OFF \
        -DWITH_TESTS=OFF \
        -DWITH_SIMULATION_MODULES=OFF \
	-DWPILIB_TARGET_WARNINGS=-Wno-maybe-uninitialized \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${ROOTFS_DIR}/usr/src/opencv-4.10.0/platforms/linux/aarch64-gnu.toolchain.cmake \
        -DARM_LINUX_SYSROOT=${ROOTFS_DIR} \
        -DCMAKE_MODULE_PATH=${SUB_STAGE_DIR}/files \
        -DOpenCV_DIR=${ROOTFS_DIR}/usr/local/frc/share/OpenCV \
        -DWITH_JAVA=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DTHREADS_PTHREAD_ARG=-pthread \
        -DCMAKE_INSTALL_PREFIX=/usr/local/frc-static \
        || exit 1
    ninja || exit 1
    env DESTDIR=${ROOTFS_DIR} ninja install || exit 1
    popd
}
build_static_wpilib build/allwpilib-static || exit 1

# executables (use static build to ensure they don't break)
sh -c 'cd build/allwpilib-static/bin && tar cf - cscore_* netconsoleTee*' | \
    sh -c "cd ${ROOTFS_DIR}/usr/local/frc/bin && tar xf -"

# pkgconfig files
install -v -d "${ROOTFS_DIR}/usr/local/frc/lib/pkgconfig"
install -m 644 ${SUB_STAGE_DIR}/files/pkgconfig/*.pc "${ROOTFS_DIR}/usr/local/frc/lib/pkgconfig"
install -v -d "${ROOTFS_DIR}/usr/local/frc-static/lib/pkgconfig"
for f in ${SUB_STAGE_DIR}/files/pkgconfig/*.pc; do
  install -m 644 $f "${ROOTFS_DIR}/usr/local/frc-static/lib/pkgconfig"
  sed -i -e 's,/usr/local/frc,/usr/local/frc-static,' "${ROOTFS_DIR}/usr/local/frc-static/lib/pkgconfig/`basename $f`"
done

# clean up frc-static
rm -rf "${ROOTFS_DIR}/usr/local/frc-static/bin"
rm -rf "${ROOTFS_DIR}/usr/local/frc-static/include"
ln -sf ../frc/include "${ROOTFS_DIR}/usr/local/frc-static/include"
rm -rf "${ROOTFS_DIR}/usr/local/frc-static/python"

# fix up frc-static opencv pkgconfig Libs.private
sed -i -e 's, -L/pi-gen[^ ]*,,g' "${ROOTFS_DIR}/usr/local/frc-static/lib/pkgconfig/opencv4.pc"

popd

ROBOTPY_REPO=https://frcmaven.wpi.edu/api/download/wpilib-python-release-2025
ROBOTPY_VERSION=2025.2.1
ROBOTPY_ARCH=aarch64

on_chroot << EOF
pip3 install --break-system-packages ${ROBOTPY_REPO}/robotpy-wpiutil/${ROBOTPY_VERSION}/robotpy_wpiutil-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
pip3 install --break-system-packages ${ROBOTPY_REPO}/robotpy-wpinet/${ROBOTPY_VERSION}/robotpy_wpinet-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
pip3 install --break-system-packages ${ROBOTPY_REPO}/pyntcore/${ROBOTPY_VERSION}/pyntcore-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
pip3 install --break-system-packages ${ROBOTPY_REPO}/robotpy-cscore/${ROBOTPY_VERSION}/robotpy_cscore-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
pip3 install --break-system-packages ${ROBOTPY_REPO}/robotpy-wpimath/${ROBOTPY_VERSION}/robotpy_wpimath-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
pip3 install --break-system-packages ${ROBOTPY_REPO}/robotpy-apriltag/${ROBOTPY_VERSION}/robotpy_apriltag-${ROBOTPY_VERSION}-cp311-cp311-linux_${ROBOTPY_ARCH}.whl
EOF

#
# Build pixy2
#
on_chroot << EOF
pushd /usr/src/pixy2/scripts
./build_libpixyusb2.sh
./build_python_demos.sh
popd
EOF

install -m 644 "${EXTRACT_DIR}/pixy2/build/libpixyusb2/libpixy2.a" "${ROOTFS_DIR}/usr/local/frc/lib/"
install -m 644 "${EXTRACT_DIR}/pixy2/build/python_demos/pixy.py" "${ROOTFS_DIR}/usr/local/lib/python3.11/dist-packages/"
install -m 755 ${EXTRACT_DIR}/pixy2/build/python_demos/_pixy.*.so "${ROOTFS_DIR}/usr/local/lib/python3.11/dist-packages/"
rm -rf "${EXTRACT_DIR}/pixy2/build"

#
# Finish up
#

# Split debug info

split_debug () {
    aarch64-linux-gnu-objcopy --only-keep-debug $1 $1.debug
    aarch64-linux-gnu-strip -g $1
    aarch64-linux-gnu-objcopy --add-gnu-debuglink=$1.debug $1
}

split_debug_so () {
    pushd $1
    for lib in *.so
    do
        split_debug $lib
    done
    popd
}

split_debug_exe () {
    pushd $1
    for exe in *
    do
        split_debug $exe
    done
    popd
}

split_debug_exe "${ROOTFS_DIR}/usr/local/frc/bin"
split_debug_so "${ROOTFS_DIR}/usr/local/frc/lib"

# Add /usr/local/frc/lib to ldconfig

install -m 644 files/ld.so.conf.d/*.conf "${ROOTFS_DIR}/etc/ld.so.conf.d/"

# Add /usr/local/frc/lib/pkgconfig to pkg-config

install -m 644 files/profile.d/*.sh "${ROOTFS_DIR}/etc/profile.d/"

# Add udev rules

install -m 644 files/rules.d/*.rules "${ROOTFS_DIR}/etc/udev/rules.d/"

on_chroot << EOF
ldconfig
EOF
