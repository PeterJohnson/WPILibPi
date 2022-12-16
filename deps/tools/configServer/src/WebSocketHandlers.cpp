// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "WebSocketHandlers.h"

#include <unistd.h>

#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include <fmt/format.h>
#include <wpi/json.h>
#include <wpi/raw_ostream.h>
#include <wpi/SmallVector.h>
#include <wpi/StringExtras.h>
#include <wpinet/raw_uv_ostream.h>
#include <wpinet/uv/Loop.h>
#include <wpinet/uv/Pipe.h>
#include <wpinet/uv/Process.h>
#include <wpinet/WebSocket.h>

#include "Application.h"
#include "NetworkSettings.h"
#include "RomiStatus.h"
#include "SystemStatus.h"
#include "UploadHelper.h"
#include "VisionSettings.h"
#include "VisionStatus.h"

namespace uv = wpi::uv;

extern bool romi;

struct WebSocketData {
  bool visionLogEnabled = false;
  bool romiLogEnabled = false;

  UploadHelper upload;

  wpi::sig::ScopedConnection sysStatusConn;
  wpi::sig::ScopedConnection sysWritableConn;
  wpi::sig::ScopedConnection visStatusConn;
  wpi::sig::ScopedConnection visLogConn;
  wpi::sig::ScopedConnection romiStatusConn;
  wpi::sig::ScopedConnection romiLogConn;
  wpi::sig::ScopedConnection romiConfigConn;
  wpi::sig::ScopedConnection cameraListConn;
  wpi::sig::ScopedConnection netSettingsConn;
  wpi::sig::ScopedConnection visSettingsConn;
  wpi::sig::ScopedConnection appSettingsConn;
};

static void SendWsText(wpi::WebSocket& ws, const wpi::json& j) {
  wpi::SmallVector<uv::Buffer, 4> toSend;
  wpi::raw_uv_ostream os{toSend, 4096};
  os << j;
  ws.SendText(toSend, [](std::span<uv::Buffer> bufs, uv::Error) {
    for (auto&& buf : bufs) buf.Deallocate();
  });
}

template <typename OnSuccessFunc, typename OnFailFunc, typename... Args>
static void RunProcess(wpi::WebSocket& ws, OnSuccessFunc success,
                       OnFailFunc fail, std::string_view file,
                       const Args&... args) {
  uv::Loop& loop = ws.GetStream().GetLoopRef();

  // create pipe to capture stderr
  auto pipe = uv::Pipe::Create(loop);
  if (auto proc = uv::Process::Spawn(
          loop, file,
          pipe ? uv::Process::StdioCreatePipe(2, *pipe, UV_WRITABLE_PIPE)
               : uv::Process::Option(),
          args...)) {
    // capture stderr output into string
    auto output = std::make_shared<std::string>();
    if (pipe) {
      pipe->StartRead();
      pipe->data.connect([output](uv::Buffer& buf, size_t len) {
        output->append(buf.base, len);
      });
      pipe->end.connect([p = pipe.get()] { p->Close(); });
    }

    // on exit, report
    proc->exited.connect(
        [ p = proc.get(), output, s = ws.shared_from_this(), fail, success ](
            int64_t status, int sig) {
          if (status != EXIT_SUCCESS) {
            SendWsText(
                *s,
                {{"type", "status"}, {"code", status}, {"message", *output}});
            fail(*s);
          } else {
            success(*s);
          }
          p->Close();
        });
  } else {
    SendWsText(ws,
               {{"type", "status"}, {"message", "could not spawn process"}});
    fail(ws);
  }
}

void InitWs(wpi::WebSocket& ws) {
  // set ws data
  auto data = std::make_shared<WebSocketData>();
  ws.SetData(data);

  // send initial system status and hook up system status updates
  auto sysStatus = SystemStatus::GetInstance();

  auto statusFunc = [&ws](const wpi::json& j) { SendWsText(ws, j); };
  statusFunc(sysStatus->GetStatusJson());
  data->sysStatusConn = sysStatus->status.connect_connection(statusFunc);

  auto writableFunc = [&ws](bool writable) {
    if (writable)
      SendWsText(ws, {{"type", "systemWritable"}});
    else
      SendWsText(ws, {{"type", "systemReadOnly"}});
  };
  writableFunc(sysStatus->GetWritable());
  data->sysWritableConn = sysStatus->writable.connect_connection(writableFunc);

  // hook up vision status updates and logging
  auto visStatus = VisionStatus::GetInstance();
  data->visStatusConn = visStatus->update.connect_connection(
      [&ws](const wpi::json& j) { SendWsText(ws, j); });
  data->visLogConn =
      visStatus->log.connect_connection([&ws](const wpi::json& j) {
        auto d = ws.GetData<WebSocketData>();
        if (d->visionLogEnabled) SendWsText(ws, j);
      });
  visStatus->UpdateStatus();
  data->cameraListConn = visStatus->cameraList.connect_connection(
      [&ws](const wpi::json& j) { SendWsText(ws, j); });
  visStatus->UpdateCameraList();

  // hook up romi status updates and logging
  if (romi) {
    SendWsText(ws, {{"type", "romiEnable"}});
    auto romiStatus = RomiStatus::GetInstance();
    data->romiStatusConn = romiStatus->update.connect_connection(
        [&ws](const wpi::json& j) { SendWsText(ws, j); });
    data->romiLogConn =
        romiStatus->log.connect_connection([&ws](const wpi::json& j) {
          auto d = ws.GetData<WebSocketData>();
          if (d->romiLogEnabled) SendWsText(ws, j);
        });
    data->romiConfigConn = romiStatus->config.connect_connection(
        [&ws](const wpi::json& j) { SendWsText(ws, j); });
    romiStatus->UpdateStatus();
    romiStatus->UpdateConfig(statusFunc);
  }

  // send initial network settings
  auto netSettings = NetworkSettings::GetInstance();
  auto netSettingsFunc = [&ws](const wpi::json& j) { SendWsText(ws, j); };
  netSettingsFunc(netSettings->GetStatusJson());
  data->netSettingsConn =
      netSettings->status.connect_connection(netSettingsFunc);

  // send initial vision settings
  auto visSettings = VisionSettings::GetInstance();
  auto visSettingsFunc = [&ws](const wpi::json& j) { SendWsText(ws, j); };
  visSettingsFunc(visSettings->GetStatusJson());
  data->visSettingsConn =
      visSettings->status.connect_connection(visSettingsFunc);

  // send initial application settings
  auto appSettings = Application::GetInstance();
  auto appSettingsFunc = [&ws](const wpi::json& j) { SendWsText(ws, j); };
  appSettingsFunc(appSettings->GetStatusJson());
  data->appSettingsConn =
      appSettings->status.connect_connection(appSettingsFunc);
}

void ProcessWsText(wpi::WebSocket& ws, std::string_view msg) {
  fmt::print(stderr, "ws: '{}'\n", msg);

  // parse
  wpi::json j;
  try {
    j = wpi::json::parse(msg, nullptr, false);
  } catch (const wpi::json::parse_error& e) {
    fmt::print(stderr, "parse error at byte {}: {}\n", e.byte, e.what());
    return;
  }

  // top level must be an object
  if (!j.is_object()) {
    wpi::errs() << "not object\n";
    return;
  }

  // type
  std::string type;
  try {
    type = j.at("type").get<std::string>();
  } catch (const wpi::json::exception& e) {
    fmt::print(stderr, "could not read type: {}\n", e.what());
    return;
  }

  fmt::print("type: {}\n", type);

  //uv::Loop& loop = ws.GetStream().GetLoopRef();

  std::string_view t(type);
  if (wpi::starts_with(t, "system")) {
    std::string_view subType = wpi::substr(t, 6);

    auto readOnlyFunc = [](wpi::WebSocket& s) {
      SendWsText(s, {{"type", "systemReadOnly"}});
    };
    auto writableFunc = [](wpi::WebSocket& s) {
      SendWsText(s, {{"type", "systemWritable"}});
    };

    if (subType == "Restart") {
      RunProcess(ws, [](wpi::WebSocket&) {}, [](wpi::WebSocket&) {},
                 "/sbin/reboot", "/sbin/reboot");
    } else if (subType == "ReadOnly") {
      RunProcess(
          ws, readOnlyFunc, writableFunc, "/bin/sh", "/bin/sh", "-c",
          "/bin/mount -o remount,ro / && /bin/mount -o remount,ro /boot");
    } else if (subType == "Writable") {
      RunProcess(
          ws, writableFunc, readOnlyFunc, "/bin/sh", "/bin/sh", "-c",
          "/bin/mount -o remount,rw / && /bin/mount -o remount,rw /boot");
    }
  } else if (wpi::starts_with(t, "vision")) {
    std::string_view subType = wpi::substr(t, 6);

    auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
      SendWsText(*s, {{"type", "status"}, {"message", msg}});
    };

    if (subType == "Up") {
      VisionStatus::GetInstance()->Up(statusFunc);
    } else if (subType == "Down") {
      VisionStatus::GetInstance()->Down(statusFunc);
    } else if (subType == "Term") {
      VisionStatus::GetInstance()->Terminate(statusFunc);
    } else if (subType == "Kill") {
      VisionStatus::GetInstance()->Kill(statusFunc);
    } else if (subType == "LogEnabled") {
      try {
        ws.GetData<WebSocketData>()->visionLogEnabled =
            j.at("value").get<bool>();
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read visionLogEnabled value: {}\n",
                   e.what());
        return;
      }
    } else if (subType == "Save") {
      try {
        VisionSettings::GetInstance()->Set(j.at("settings"), statusFunc);
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read visionSave value: {}\n", e.what());
        return;
      }
    }
  } else if (wpi::starts_with(t,"romi") && romi) {
    std::string_view subType = wpi::substr(t, 4);

    auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
      SendWsText(*s, {{"type", "status"}, {"message", msg}});
    };

    if (subType == "Up") {
      RomiStatus::GetInstance()->Up(statusFunc);
    } else if (subType == "Down") {
      RomiStatus::GetInstance()->Down(statusFunc);
    } else if (subType == "Term") {
      RomiStatus::GetInstance()->Terminate(statusFunc);
    } else if (subType == "Kill") {
      RomiStatus::GetInstance()->Kill(statusFunc);
    } else if (subType == "FirmwareUpdate") {
      RomiStatus::GetInstance()->FirmwareUpdate(statusFunc);
    } else if (subType == "LogEnabled") {
      try {
        ws.GetData<WebSocketData>()->romiLogEnabled = j.at("value").get<bool>();
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read romiLogEnabled value: {}\n",
                   e.what());
        return;
      }
    } else if (subType == "SaveExternalIOConfig") {
      try {
        RomiStatus::GetInstance()->SaveConfig(j.at("romiConfig"), true, statusFunc);
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read romiConfig value: {}\n", e.what());
        return;
      }
    } else if (subType == "SaveGyroCalibration") {
      try {
        RomiStatus::GetInstance()->SaveConfig(j.at("romiConfig"), false, statusFunc);
      }
      catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read romiConfig value: {}\n", e.what());
        return;
      }
    } else if (subType == "ServiceStartUpload") {
      auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
        SendWsText(*s, {{"type", "status"}, {"message", msg}});
      };
      auto d = ws.GetData<WebSocketData>();
      d->upload.Open(EXEC_HOME "/romiUploadXXXXXX", false, statusFunc);
    }
    else if (subType == "ServiceFinishUpload") {
      auto d = ws.GetData<WebSocketData>();

      if (fchown(d->upload.GetFD(), APP_UID, APP_GID) == -1) {
        fmt::print(stderr, "could not change file ownership: {}\n",
                   std::strerror(errno));
      }
      d->upload.Close();

      std::string filename;
      try {
        filename = j.at("fileName").get<std::string>();
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read fileName value: {}\n", e.what());
        unlink(d->upload.GetFilename());
        return;
      }

      auto pathname = fmt::format("{}/{}", EXEC_HOME, filename);
      if (unlink(pathname.c_str()) == -1) {
        fmt::print(stderr, "could not remove file: {}\n", std::strerror(errno));
      }

      // Rename temporary file to new file
      if (rename(d->upload.GetFilename(), pathname.c_str()) == -1) {
        fmt::print(stderr, "could not rename: {}\n", std::strerror(errno));
      }

      auto installSuccess = [sf = statusFunc](wpi::WebSocket& s) {
        auto d = s.GetData<WebSocketData>();
        unlink(d->upload.GetFilename());
        SendWsText(s, {{"type", "romiServiceUploadComplete"}, {"success", true}});
        RomiStatus::GetInstance()->Up(sf);
      };

      auto installFailure = [sf = statusFunc](wpi::WebSocket& s) {
        auto d = s.GetData<WebSocketData>();
        unlink(d->upload.GetFilename());
        fmt::print(stderr, "could not install service\n");
        SendWsText(s, {{"type", "romiServiceUploadComplete"}});
        RomiStatus::GetInstance()->Up(sf);
      };

      RomiStatus::GetInstance()->Down(statusFunc);
      RunProcess(ws, installSuccess, installFailure, NODE_HOME "/npm",
                 uv::Process::Uid(APP_UID), uv::Process::Gid(APP_GID),
                 uv::Process::Cwd(EXEC_HOME),
                 uv::Process::Env("PATH=$PATH:" NODE_HOME),
                 NODE_HOME "/npm",
                 "install", "-g", pathname);

    }
  } else if (t == "networkSave") {
    auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
      SendWsText(*s, {{"type", "status"}, {"message", msg}});
    };
    try {
      NetworkSettings::Mode mode;
      auto& approach = j.at("networkApproach").get_ref<const std::string&>();
      if (approach == "dhcp")
        mode = NetworkSettings::kDhcp;
      else if (approach == "static")
        mode = NetworkSettings::kStatic;
      else if (approach == "dhcp-fallback")
        mode = NetworkSettings::kDhcpStatic;
      else {
        fmt::print(stderr, "could not understand networkApproach value: {}\n",
                   approach);
        return;
      }

      NetworkSettings::Mode wifiMode;
      auto& wifiApproach =
          j.at("wifiNetworkApproach").get_ref<const std::string&>();
      if (wifiApproach == "dhcp")
        wifiMode = NetworkSettings::kDhcp;
      else if (wifiApproach == "static")
        wifiMode = NetworkSettings::kStatic;
      else if (wifiApproach == "dhcp-fallback")
        wifiMode = NetworkSettings::kDhcpStatic;
      else {
        fmt::print(stderr,
                   "could not understand wifiNetworkApproach value: {}\n",
                   wifiApproach);
        return;
      }

      NetworkSettings::WifiMode wifiAPMode;
      auto& wifiAPModeStr = j.at("wifiMode").get_ref<const std::string&>();
      if (wifiAPModeStr == "bridge")
        wifiAPMode = NetworkSettings::kBridge;
      else if (wifiAPModeStr == "access-point")
        wifiAPMode = NetworkSettings::kAccessPoint;
      else {
        fmt::print(stderr, "could not understand wifiMode value: {}\n",
                   wifiAPModeStr);
        return;
      }

      NetworkSettings::GetInstance()->Set(
          mode, j.at("networkAddress").get_ref<const std::string&>(),
          j.at("networkMask").get_ref<const std::string&>(),
          j.at("networkGateway").get_ref<const std::string&>(),
          j.at("networkDNS").get_ref<const std::string&>(), wifiAPMode,
          j.at("wifiChannel").get<int>(),
          j.at("wifiSsid").get_ref<const std::string&>(),
          j.at("wifiWpa2").get_ref<const std::string&>(), wifiMode,
          j.at("wifiNetworkAddress").get_ref<const std::string&>(),
          j.at("wifiNetworkMask").get_ref<const std::string&>(),
          j.at("wifiNetworkGateway").get_ref<const std::string&>(),
          j.at("wifiNetworkDNS").get_ref<const std::string&>(), statusFunc);
    } catch (const wpi::json::exception& e) {
      fmt::print(stderr, "could not read networkSave value: {}\n", e.what());
      return;
    }
  } else if (wpi::starts_with(t, "application")) {
    std::string_view subType = wpi::substr(t, 11);

    auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
      SendWsText(*s, {{"type", "status"}, {"message", msg}});
    };

    std::string appType;
    try {
      appType = j.at("applicationType").get<std::string>();
    } catch (const wpi::json::exception& e) {
      fmt::print(stderr, "could not read applicationSave value: {}\n",
                 e.what());
      return;
    }

    if (subType == "Save") {
      Application::GetInstance()->Set(appType, statusFunc);
    } else if (subType == "StartUpload") {
      auto d = ws.GetData<WebSocketData>();
      d->upload.Open(EXEC_HOME "/appUploadXXXXXX",
                     wpi::ends_with(appType, "python"), statusFunc);
    } else if (subType == "FinishUpload") {
      auto d = ws.GetData<WebSocketData>();
      if (d->upload)
        Application::GetInstance()->FinishUpload(appType, d->upload,
                                                 statusFunc);
      SendWsText(ws, {{"type", "applicationSaveComplete"}});
    }
  } else if (wpi::starts_with(t, "file")) {
    std::string_view subType = wpi::substr(t, 4);
    if (subType == "StartUpload") {
      auto statusFunc = [s = ws.shared_from_this()](std::string_view msg) {
        SendWsText(*s, {{"type", "status"}, {"message", msg}});
      };
      auto d = ws.GetData<WebSocketData>();
      d->upload.Open(EXEC_HOME "/fileUploadXXXXXX", false, statusFunc);
    } else if (subType == "FinishUpload") {
      auto d = ws.GetData<WebSocketData>();

      // change ownership
      if (fchown(d->upload.GetFD(), APP_UID, APP_GID) == -1) {
        fmt::print(stderr, "could not change file ownership: {}\n",
                   std::strerror(errno));
      }
      d->upload.Close();

      bool extract;
      try {
        extract = j.at("extract").get<bool>();
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read extract value: {}\n", e.what());
        unlink(d->upload.GetFilename());
        return;
      }

      std::string filename;
      try {
        filename = j.at("fileName").get<std::string>();
      } catch (const wpi::json::exception& e) {
        fmt::print(stderr, "could not read fileName value: {}\n", e.what());
        unlink(d->upload.GetFilename());
        return;
      }

      auto extractSuccess = [](wpi::WebSocket& s) {
        auto d = s.GetData<WebSocketData>();
        unlink(d->upload.GetFilename());
        SendWsText(s, {{"type", "fileUploadComplete"}, {"success", true}});
      };
      auto extractFailure = [](wpi::WebSocket& s) {
        auto d = s.GetData<WebSocketData>();
        unlink(d->upload.GetFilename());
        fmt::print(stderr, "could not extract file\n");
        SendWsText(s, {{"type", "fileUploadComplete"}});
      };

      if (extract && wpi::ends_with(filename, "tar.gz")) {
        RunProcess(ws, extractSuccess, extractFailure, "/bin/tar",
                   uv::Process::Uid(APP_UID), uv::Process::Gid(APP_GID),
                   uv::Process::Cwd(EXEC_HOME), "/bin/tar", "xzf",
                   d->upload.GetFilename());
      } else if (extract && wpi::ends_with(filename, "zip")) {
        d->upload.Close();
        RunProcess(ws, extractSuccess, extractFailure, "/usr/bin/unzip",
                   uv::Process::Uid(APP_UID), uv::Process::Gid(APP_GID),
                   uv::Process::Cwd(EXEC_HOME), "/usr/bin/unzip",
                   d->upload.GetFilename());
      } else {
        auto pathname = fmt::format("{}/{}", EXEC_HOME, filename);
        if (unlink(pathname.c_str()) == -1) {
          fmt::print(stderr, "could not remove file: {}\n",
                     std::strerror(errno));
        }

        // rename temporary file to new file
        if (rename(d->upload.GetFilename(), pathname.c_str()) == -1) {
          fmt::print(stderr, "could not rename: {}\n", std::strerror(errno));
        }

        SendWsText(ws, {{"type", "fileUploadComplete"}});
      }
    }
  }
}

void ProcessWsBinary(wpi::WebSocket& ws, std::span<const uint8_t> msg) {
  auto d = ws.GetData<WebSocketData>();
  if (d->upload) d->upload.Write(msg);
}
