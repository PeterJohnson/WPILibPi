// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#ifndef RPICONFIGSERVER_ROMISTATUS_H_
#define RPICONFIGSERVER_ROMISTATUS_H_

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <cscore.h>
#include <wpi/Signal.h>
#include <wpinet/uv/Loop.h>

namespace wpi {
class json;

namespace uv {
class Buffer;
}  // namespace uv
}  // namespace wpi

class RomiStatus {
  struct private_init {};

 public:
  explicit RomiStatus(const private_init&) {}
  RomiStatus(const RomiStatus&) = delete;
  RomiStatus& operator=(const RomiStatus&) = delete;

  void SetLoop(std::shared_ptr<wpi::uv::Loop> loop);

  void Up(std::function<void(std::string_view)> onFail);
  void Down(std::function<void(std::string_view)> onFail);
  void Terminate(std::function<void(std::string_view)> onFail);
  void Kill(std::function<void(std::string_view)> onFail);

  void UpdateStatus();
  void ConsoleLog(wpi::uv::Buffer& buf, size_t len);

  void FirmwareUpdate(std::function<void(std::string_view)> onFail);

  void UpdateConfig(std::function<void(std::string_view)> onFail);

  void SaveConfig(const wpi::json& data, bool restartService, std::function<void(std::string_view)> onFail);

  wpi::sig::Signal<const wpi::json&> update;
  wpi::sig::Signal<const wpi::json&> log;
  wpi::sig::Signal<const wpi::json&> config;

  static std::shared_ptr<RomiStatus> GetInstance();

 private:
  void RunSvc(const char* cmd, std::function<void(std::string_view)> onFail);
  wpi::json ReadRomiConfigFile(std::function<void(std::string_view)> onFail);
  wpi::json GetConfigJson(std::function<void(std::string_view)> onFail);

  std::shared_ptr<wpi::uv::Loop> m_loop;
};

#endif  // RPICONFIGSERVER_VISIONSTATUS_H_
