# For a desktop build with the allwpilib source tree:
#ALLWPILIB=/home/peter/project/frc/allwpilib
#DEPS_CFLAGS= \
	-I${ALLWPILIB}/wpiutil/src/main/native/include \
	-I${ALLWPILIB}/wpinet/src/main/native/include \
	-I${ALLWPILIB}/cameraserver/src/main/native/include \
	-I${ALLWPILIB}/cscore/src/main/native/include \
	-I${ALLWPILIB}/ntcore/src/main/native/include \
	-I${ALLWPILIB}/build-ninja/ntcore/generated/main/native/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/llvm/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/sigslot/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/fmtlib/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/json/include \
	-I${ALLWPILIB}/wpinet/src/main/native/thirdparty/libuv/include
#DEPS_LIBS=-L${ALLWPILIB}/build-ninja/lib -lcameraserverd -lntcored -lcscored -lwpinetd -lwpiutild

DEPS_CFLAGS?=$(shell pkg-config --cflags cameraserver ntcore wpiutil)
DEPS_LIBS?=$(shell pkg-config --libs --static cameraserver ntcore wpiutil)
CXXFLAGS?=-std=c++20

.PHONY: all clean

all: multiCameraServer

clean:
	rm -f multiCameraServer

multiCameraServer: src/multiCameraServer.cpp
	${CXX} -pthread -g -o $@ ${CXXFLAGS} ${DEPS_CFLAGS} $^ ${DEPS_LIBS}
