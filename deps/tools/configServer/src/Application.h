// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#ifndef RPICONFIGSERVER_APPLICATION_H_
#define RPICONFIGSERVER_APPLICATION_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <wpi/Signal.h>

namespace wpi {
class json;
}  // namespace wpi

class UploadHelper;

class Application {
  struct private_init {};

 public:
  explicit Application(const private_init&) {}
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  void Set(std::string_view appType,
           std::function<void(std::string_view)> onFail);

  void FinishUpload(std::string_view appType, UploadHelper& helper,
                    std::function<void(std::string_view)> onFail);

  void UpdateStatus();

  wpi::json GetStatusJson();

  wpi::sig::Signal<const wpi::json&> status;

  static std::shared_ptr<Application> GetInstance();
};

#endif  // RPICONFIGSERVER_APPLICATION_H_
