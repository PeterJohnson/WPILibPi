// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "VisionStatus.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string_view>

#include <cscore.h>
#include <fmt/format.h>
#include <wpi/SmallString.h>
#include <wpi/StringExtras.h>
#include <wpi/fmt/raw_ostream.h>
#include <wpi/json.h>
#include <wpi/raw_ostream.h>
#include <wpinet/uv/Buffer.h>
#include <wpinet/uv/FsEvent.h>
#include <wpinet/uv/Pipe.h>
#include <wpinet/uv/Process.h>
#include <wpinet/uv/Timer.h>
#include <wpinet/uv/Work.h>

namespace uv = wpi::uv;

#define SERVICE "/service/camera"

std::shared_ptr<VisionStatus> VisionStatus::GetInstance() {
  static auto visStatus = std::make_shared<VisionStatus>(private_init{});
  return visStatus;
}

void VisionStatus::SetLoop(std::shared_ptr<wpi::uv::Loop> loop) {
  m_loop = std::move(loop);

  auto refreshTimer = wpi::uv::Timer::Create(m_loop);
  refreshTimer->timeout.connect([this] { RefreshCameraList(); });
  refreshTimer->Unreference();

  auto devEvents = wpi::uv::FsEvent::Create(m_loop);
  devEvents->fsEvent.connect([refreshTimer](const char* fn, int flags) {
    if (wpi::starts_with(fn, "video"))
      refreshTimer->Start(wpi::uv::Timer::Time(200));
  });
  devEvents->Start("/dev");
  devEvents->Unreference();

  refreshTimer->Start(wpi::uv::Timer::Time(200));
}

void VisionStatus::UpdateCameraList() {
  wpi::json j = {{"type", "cameraList"}, {"cameras", wpi::json::array()}};
  auto& cams = j["cameras"];
  for (const auto& caminfo : m_cameraInfo) {
    wpi::json cam = {{"dev", caminfo.info.dev},
                     {"path", caminfo.info.path},
                     {"name", caminfo.info.name},
                     {"otherPaths", wpi::json::array()},
                     {"modes", wpi::json::array()}};

    auto& otherPaths = cam["otherPaths"];
    for (const auto& path : caminfo.info.otherPaths)
      otherPaths.emplace_back(path);

    auto& modes = cam["modes"];
    for (const auto& mode : caminfo.modes) {
      wpi::json jmode;

      std::string_view pixelFormatStr;
      switch (mode.pixelFormat) {
        case cs::VideoMode::kMJPEG:
          pixelFormatStr = "mjpeg";
          break;
        case cs::VideoMode::kYUYV:
          pixelFormatStr = "yuyv";
          break;
        case cs::VideoMode::kRGB565:
          pixelFormatStr = "rgb565";
          break;
        case cs::VideoMode::kBGR:
          pixelFormatStr = "bgr";
          break;
        case cs::VideoMode::kGray:
          pixelFormatStr = "gray";
          break;
        default:
          continue;
      }
      jmode.emplace("pixelFormat", pixelFormatStr);

      jmode.emplace("width", mode.width);
      jmode.emplace("height", mode.height);
      jmode.emplace("fps", mode.fps);

      modes.emplace_back(jmode);
    }
    cams.emplace_back(cam);
  }
  cameraList(j);
}

void VisionStatus::RefreshCameraList() {
  struct RefreshCameraWorkReq : public uv::WorkReq {
    std::vector<CameraInfo> cameraInfo;
  };
  auto workReq = std::make_shared<RefreshCameraWorkReq>();
  workReq->work.connect([r = workReq.get()] {
    CS_Status status = 0;
    for (auto&& caminfo : cs::EnumerateUsbCameras(&status)) {
      cs::UsbCamera camera{"usbcam", caminfo.dev};
      r->cameraInfo.emplace_back();
      r->cameraInfo.back().info = std::move(caminfo);
      r->cameraInfo.back().modes = camera.EnumerateVideoModes();
    }
  });
  workReq->afterWork.connect([ this, r = workReq.get() ] {
    m_cameraInfo = std::move(r->cameraInfo);
    UpdateCameraList();
  });
  uv::QueueWork(m_loop, workReq);
}

void VisionStatus::RunSvc(const char* cmd,
                          std::function<void(std::string_view)> onFail) {
  struct SvcWorkReq : public uv::WorkReq {
    SvcWorkReq(const char* cmd_, std::function<void(std::string_view)> onFail_)
        : cmd(cmd_), onFail(onFail_) {}
    const char* cmd;
    std::function<void(std::string_view)> onFail;
    wpi::SmallString<128> err;
  };

  auto workReq = std::make_shared<SvcWorkReq>(cmd, onFail);
  workReq->work.connect([r = workReq.get()] {
    int fd = open(SERVICE "/supervise/control", O_WRONLY | O_NDELAY);
    if (fd == -1) {
      wpi::raw_svector_ostream os(r->err);
      if (errno == ENXIO)
        wpi::print(os, "unable to control service: supervise not running");
      else
        wpi::print(os, "unable to control service: {}", std::strerror(errno));
    } else {
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
      if (write(fd, r->cmd, std::strlen(r->cmd)) == -1) {
        wpi::raw_svector_ostream os(r->err);
        wpi::print(os, "error writing command: {}", std::strerror(errno));
      }
      close(fd);
    }
  });
  workReq->afterWork.connect([r = workReq.get()] {
    if (r->onFail && !r->err.empty()) r->onFail(r->err.str());
  });

  uv::QueueWork(m_loop, workReq);
}

void VisionStatus::Up(std::function<void(std::string_view)> onFail) {
  RunSvc("u", onFail);
  UpdateStatus();
}

void VisionStatus::Down(std::function<void(std::string_view)> onFail) {
  RunSvc("d", onFail);
  UpdateStatus();
}

void VisionStatus::Terminate(std::function<void(std::string_view)> onFail) {
  RunSvc("t", onFail);
  UpdateStatus();
}

void VisionStatus::Kill(std::function<void(std::string_view)> onFail) {
  RunSvc("k", onFail);
  UpdateStatus();
}

void VisionStatus::UpdateStatus() {
  struct StatusWorkReq : public uv::WorkReq {
    bool enabled = false;
    wpi::SmallString<128> status;
  };

  auto workReq = std::make_shared<StatusWorkReq>();

  workReq->work.connect([r = workReq.get()] {
    wpi::raw_svector_ostream os(r->status);

    // check to make sure supervise is running
    int fd = open(SERVICE "/supervise/ok", O_WRONLY | O_NDELAY);
    if (fd == -1) {
      if (errno == ENXIO)
        wpi::print(os, "supervise not running");
      else
        wpi::print(os, "unable to open supervise/ok: {}", std::strerror(errno));
      return;
    }
    close(fd);

    // read the status data
    fd = open(SERVICE "/supervise/status", O_RDONLY | O_NDELAY);
    if (fd == -1) {
      wpi::print(os, "unable to open supervise/status: {}",
                 std::strerror(errno));
      return;
    }
    uint8_t status[18];
    int nr = read(fd, status, sizeof status);
    close(fd);
    if (nr < static_cast<int>(sizeof status)) {
      wpi::print(os, "unable to read supervise/status: ");
      if (nr == -1)
        wpi::print(os, std::strerror(errno));
      else
        wpi::print(os, "bad format");
      return;
    }

    // decode the status data (based on daemontools svstat.c)
    uint32_t pid = (static_cast<uint32_t>(status[15]) << 24) |
                   (static_cast<uint32_t>(status[14]) << 16) |
                   (static_cast<uint32_t>(status[13]) << 8) |
                   (static_cast<uint32_t>(status[12]));
    bool paused = status[16];
    auto want = status[17];
    uint64_t when = (static_cast<uint64_t>(status[0]) << 56) |
                    (static_cast<uint64_t>(status[1]) << 48) |
                    (static_cast<uint64_t>(status[2]) << 40) |
                    (static_cast<uint64_t>(status[3]) << 32) |
                    (static_cast<uint64_t>(status[4]) << 24) |
                    (static_cast<uint64_t>(status[5]) << 16) |
                    (static_cast<uint64_t>(status[6]) << 8) |
                    (static_cast<uint64_t>(status[7]));

    // constant is from daemontools tai.h
    uint64_t now =
        4611686018427387914ULL + static_cast<uint64_t>(std::time(nullptr));
    if (now >= when)
      when = now - when;
    else
      when = 0;

    // convert to status string
    if (pid)
      wpi::print(os, "up (pid {}) ", pid);
    else
      wpi::print(os, "down ");
    wpi::print(os, "{} seconds", when);
    if (pid && paused) wpi::print(os, ", paused");
    if (!pid && want == 'u') wpi::print(os, ", want up");
    if (pid && want == 'd') wpi::print(os, ", want down");

    if (pid) r->enabled = true;
  });

  workReq->afterWork.connect([this, r = workReq.get()] {
    wpi::json j = {{"type", "visionStatus"},
                   {"visionServiceEnabled", r->enabled},
                   {"visionServiceStatus", r->status.str()}};
    update(j);
  });

  uv::QueueWork(m_loop, workReq);
}

void VisionStatus::ConsoleLog(uv::Buffer& buf, size_t len) {
  wpi::json j = {{"type", "visionLog"},
                 {"data", std::string_view(buf.base, len)}};
  log(j);
}
