IMG_VERSION?=$(shell git describe)

# For a desktop build with the allwpilib source tree:
#ALLWPILIB=/home/peter/project/frc/allwpilib
#DEPS_CFLAGS= \
	-I${ALLWPILIB}/wpiutil/src/main/native/include \
	-I${ALLWPILIB}/wpinet/src/main/native/include \
	-I${ALLWPILIB}/cscore/src/main/native/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/expected/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/llvm/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/sigslot/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/fmtlib/include \
	-I${ALLWPILIB}/wpiutil/src/main/native/thirdparty/json/include \
	-I${ALLWPILIB}/wpinet/src/main/native/thirdparty/libuv/include
#DEPS_LIBS=-L${ALLWPILIB}/build-ninja/lib -lcscored -lwpinetd -lwpiutild

DEPS_CFLAGS?=$(shell pkg-config --cflags cscore wpinet wpiutil)
DEPS_LIBS?=$(shell pkg-config --libs --static cscore wpinet wpiutil)
EXEC_HOME?=/home/pi
FRC_JSON?=/boot/frc.json
DHCPCD_CONF?=/boot/dhcpcd.conf
WPA_SUPPLICANT_CONF?=/boot/wpa_supplicant_wpilibpi.conf
DNSMASQ_CONF?=/etc/dnsmasq.d/wpilib.conf
HOSTAPD_CONF?=/etc/hostapd/hostapd.conf
APP_UID?=1000
APP_GID?=1000
ROMI_JSON?=/boot/romi.json
NODE_HOME?=/home/pi/.nvm/versions/node/v14.15.0/bin
CXXFLAGS?=-std=c++20

.PHONY: all clean install
.SUFFIXES:

all: configServer

clean:
	rm -f configServer
	rm -f src/*.o
	rm -f src/resources/*.o

install:
	svc -d /service/configServer
	install -m 755 configServer /usr/local/sbin/
	svc -u /service/configServer

OBJS= \
    src/main.o \
    src/Application.o \
    src/MyHttpConnection.o \
    src/NetworkSettings.o \
    src/RomiStatus.o \
    src/SystemStatus.o \
    src/UploadHelper.o \
    src/VisionSettings.o \
    src/VisionStatus.o \
    src/WebSocketHandlers.o \
    src/resources/index.html.o \
    src/resources/frcvision.css.o \
    src/resources/frcvision.js.o \
    src/resources/romi_ext_io.png.o

configServer: ${OBJS}
	${CXX} -pthread -g -o $@ ${CXXFLAGS} $^ ${DEPS_LIBS}

%.o: %.cpp
	${CXX} -g -O -Wall -c -o $@ \
	    ${CXXFLAGS} \
	    ${DEPS_CFLAGS} \
	    '-DEXEC_HOME="${EXEC_HOME}"' \
	    '-DFRC_JSON="${FRC_JSON}"' \
	    '-DDHCPCD_CONF="${DHCPCD_CONF}"' \
	    '-DWPA_SUPPLICANT_CONF="${WPA_SUPPLICANT_CONF}"' \
	    '-DDNSMASQ_CONF="${DNSMASQ_CONF}"' \
	    '-DHOSTAPD_CONF="${HOSTAPD_CONF}"' \
	    '-DAPP_UID=${APP_UID}' \
	    '-DAPP_GID=${APP_GID}' \
	    '-DROMI_JSON="${ROMI_JSON}"' \
	    '-DNODE_HOME="${NODE_HOME}"' \
	    $<

%.html.cpp: %.html
	env IMG_VERSION=${IMG_VERSION} ./gen_resource.py $@ $<

%.css.cpp: %.css
	env IMG_VERSION=${IMG_VERSION} ./gen_resource.py $@ $<

%.js.cpp: %.js
	env IMG_VERSION=${IMG_VERSION} ./gen_resource.py $@ $<

%.png.cpp: %.png
	env IMG_VERSION=${IMG_VERSION} ./gen_resource.py $@ $<
