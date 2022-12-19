// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <networktables/NetworkTableInstance.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/StringExtras.h>

#include "cameraserver/CameraServer.h"

/*
   JSON format:
   {
       "team": <team number>,
       "ntmode": <"client" or "server", "client" if unspecified>
       "cameras": [
           {
               "name": <camera name>
               "path": <path, e.g. "/dev/video0">
               "pixel format": <"MJPEG", "YUYV", etc>   // optional
               "width": <video mode width>              // optional
               "height": <video mode height>            // optional
               "fps": <video mode fps>                  // optional
               "brightness": <percentage brightness>    // optional
               "white balance": <"auto", "hold", value> // optional
               "exposure": <"auto", "hold", value>      // optional
               "properties": [                          // optional
                   {
                       "name": <property name>
                       "value": <property value>
                   }
               ],
               "stream": {                              // optional
                   "properties": [
                       {
                           "name": <stream property name>
                           "value": <stream property value>
                       }
                   ]
               }
           }
       ]
       "switched cameras": [
           {
               "name": <virtual camera name>
               "key": <network table key used for selection>
               // if NT value is a string, it's treated as a name
               // if NT value is a double, it's treated as an integer index
           }
       ]
   }
 */

#ifdef __RASPBIAN__
static const char* configFile = "/boot/frc.json";
#else
static const char* configFile = "frc.json";
#endif

namespace {

unsigned int team;
bool server = false;

struct CameraConfig {
  std::string name;
  std::string path;
  wpi::json config;
  wpi::json streamConfig;
};

struct SwitchedCameraConfig {
  std::string name;
  std::string key;
};

std::vector<CameraConfig> cameraConfigs;
std::vector<SwitchedCameraConfig> switchedCameraConfigs;
std::vector<cs::VideoSource> cameras;

void ParseErrorV(fmt::string_view format, fmt::format_args args) {
  fmt::print(stderr, "config error in '{}': ", configFile);
  fmt::vprint(stderr, format, args);
  fmt::print(stderr, "\n");
}

template <typename... Args>
void ParseError(fmt::string_view format, Args&&... args) {
  ParseErrorV(format, fmt::make_format_args(args...));
}

bool ReadCameraConfig(const wpi::json& config) {
  CameraConfig c;

  // name
  try {
    c.name = config.at("name").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError("could not read camera name: {}", e.what());
    return false;
  }

  // path
  try {
    c.path = config.at("path").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError("camera '{}': could not read path: {}", c.name, e.what());
    return false;
  }

  // stream properties
  if (config.count("stream") != 0) c.streamConfig = config.at("stream");

  c.config = config;

  cameraConfigs.emplace_back(std::move(c));
  return true;
}

bool ReadSwitchedCameraConfig(const wpi::json& config) {
  SwitchedCameraConfig c;

  // name
  try {
    c.name = config.at("name").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError("could not read switched camera name: {}", e.what());
    return false;
  }

  // key
  try {
    c.key = config.at("key").get<std::string>();
  } catch (const wpi::json::exception& e) {
    ParseError("switched camera '{}': could not read key: {}", c.name,
               e.what());
    return false;
  }

  switchedCameraConfigs.emplace_back(std::move(c));
  return true;
}

bool ReadConfig() {
  // open config file
  std::error_code ec;
  wpi::raw_fd_istream is(configFile, ec);
  if (ec) {
    fmt::print(stderr, "could not open '{}': {}", configFile, ec.message());
    return false;
  }

  // parse file
  wpi::json j;
  try {
    j = wpi::json::parse(is);
  } catch (const wpi::json::parse_error& e) {
    ParseError("byte {}: {}", e.byte, e.what());
    return false;
  }

  // top level must be an object
  if (!j.is_object()) {
    ParseError("must be JSON object");
    return false;
  }

  // team number
  try {
    team = j.at("team").get<unsigned int>();
  } catch (const wpi::json::exception& e) {
    ParseError("could not read team number: {}", e.what());
    return false;
  }

  // ntmode (optional)
  if (j.count("ntmode") != 0) {
    try {
      auto str = j.at("ntmode").get<std::string>();
      if (wpi::equals_lower(str, "client")) {
        server = false;
      } else if (wpi::equals_lower(str, "server")) {
        server = true;
      } else {
        ParseError("could not understand ntmode value '{}'", str);
      }
    } catch (const wpi::json::exception& e) {
      ParseError("could not read ntmode: {}", e.what());
    }
  }

  // cameras
  try {
    for (auto&& camera : j.at("cameras")) {
      if (!ReadCameraConfig(camera)) return false;
    }
  } catch (const wpi::json::exception& e) {
    ParseError("could not read cameras: {}", e.what());
    return false;
  }

  // switched cameras (optional)
  if (j.count("switched cameras") != 0) {
    try {
      for (auto&& camera : j.at("switched cameras")) {
        if (!ReadSwitchedCameraConfig(camera)) return false;
      }
    } catch (const wpi::json::exception& e) {
      ParseError("could not read switched cameras: {}", e.what());
      return false;
    }
  }

  return true;
}

cs::UsbCamera StartCamera(const CameraConfig& config) {
  fmt::print("Starting camera '{}' on {}\n", config.name, config.path);
  cs::UsbCamera camera{config.name, config.path};
  auto server = frc::CameraServer::StartAutomaticCapture(camera);

  camera.SetConfigJson(config.config);
  camera.SetConnectionStrategy(cs::VideoSource::kConnectionKeepOpen);

  if (config.streamConfig.is_object())
    server.SetConfigJson(config.streamConfig);

  return camera;
}

cs::MjpegServer StartSwitchedCamera(const SwitchedCameraConfig& config) {
  fmt::print("Starting switched camera '{}' on {}\n", config.name, config.key);
  auto server = frc::CameraServer::AddSwitchedCamera(config.name);

  auto inst = nt::NetworkTableInstance::GetDefault();
  inst.AddListener(
    inst.GetTopic(config.key),
    nt::EventFlags::kImmediate | nt::EventFlags::kValueAll,
    [server](const auto& event) mutable {
      if (auto data = event.GetValueEventData()) {
        if (data->value.IsInteger()) {
          int i = data->value.GetInteger();
          if (i >= 0 && i < cameras.size()) server.SetSource(cameras[i]);
        } else if (data->value.IsDouble()) {
          int i = data->value.GetDouble();
          if (i >= 0 && i < cameras.size()) server.SetSource(cameras[i]);
        } else if (data->value.IsString()) {
          auto str = data->value.GetString();
          for (int i = 0; i < cameraConfigs.size(); ++i) {
            if (str == cameraConfigs[i].name) {
              server.SetSource(cameras[i]);
              break;
            }
          }
        }
      }
    });

  return server;
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc >= 2) configFile = argv[1];

  // read configuration
  if (!ReadConfig()) return EXIT_FAILURE;

  // start NetworkTables
  auto ntinst = nt::NetworkTableInstance::GetDefault();
  if (server) {
    fmt::print("Setting up NetworkTables server\n");
    ntinst.StartServer();
  } else {
    fmt::print("Setting up NetworkTables client for team {}\n", team);
    ntinst.StartClient4("multiCameraServer");
    ntinst.SetServerTeam(team);
    ntinst.StartDSClient();
  }

  // start cameras
  for (const auto& config : cameraConfigs)
    cameras.emplace_back(StartCamera(config));

  // start switched cameras
  for (const auto& config : switchedCameraConfigs) StartSwitchedCamera(config);

  // loop forever
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(10));
}
