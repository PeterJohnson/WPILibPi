// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#ifndef RPICONFIGSERVER_NETWORKSETTINGS_H_
#define RPICONFIGSERVER_NETWORKSETTINGS_H_

#include <functional>
#include <memory>
#include <string_view>

#include <wpi/Signal.h>
#include <wpinet/uv/Loop.h>

namespace wpi {
class json;
}  // namespace wpi

class NetworkSettings {
  struct private_init {};

 public:
  explicit NetworkSettings(const private_init&) {}
  NetworkSettings(const NetworkSettings&) = delete;
  NetworkSettings& operator=(const NetworkSettings&) = delete;

  void SetLoop(std::shared_ptr<wpi::uv::Loop> loop) {
    m_loop = std::move(loop);
  }

  enum WifiMode { kBridge, kAccessPoint };
  enum Mode { kDhcp, kStatic, kDhcpStatic };

  void Set(Mode mode, std::string_view address, std::string_view mask,
           std::string_view gateway, std::string_view dns,
           WifiMode wifiAPMode, int wifiChannel, std::string_view wifiSsid,
           std::string_view wifiWpa2, Mode wifiMode,
           std::string_view wifiAddress, std::string_view wifiMask,
           std::string_view wifiGateway, std::string_view wifiDns,
           std::function<void(std::string_view)> onFail);

  void UpdateStatus();

  wpi::json GetStatusJson();

  wpi::sig::Signal<const wpi::json&> status;

  static std::shared_ptr<NetworkSettings> GetInstance();

 private:
  std::shared_ptr<wpi::uv::Loop> m_loop;
};

#endif  // RPICONFIGSERVER_NETWORKSETTINGS_H_
