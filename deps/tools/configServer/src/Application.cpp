// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "Application.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <wpi/SmallString.h>
#include <wpi/StringExtras.h>
#include <wpi/fmt/raw_ostream.h>
#include <wpi/fs.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>

#include "UploadHelper.h"
#include "VisionStatus.h"

#define TYPE_TAG "### TYPE:"

std::shared_ptr<Application> Application::GetInstance() {
  static auto inst = std::make_shared<Application>(private_init{});
  return inst;
}

void Application::Set(std::string_view appType,
                      std::function<void(std::string_view)> onFail) {
  std::string_view appDir;
  std::string_view appEnv;
  std::string_view appCommand;

  if (appType == "builtin") {
    appCommand = "/usr/local/frc/bin/multiCameraServer";
  } else if (appType == "example-java") {
    appDir = "examples/java-multiCameraServer";
    appCommand =
        "env LD_LIBRARY_PATH=/usr/local/frc/lib java -jar "
        "build/libs/java-multiCameraServer-all.jar";
  } else if (appType == "example-cpp") {
    appDir = "examples/cpp-multiCameraServer";
    appCommand = "./multiCameraServerExample";
  } else if (appType == "example-python") {
    appDir = "examples/python-multiCameraServer";
    appEnv = "export PYTHONUNBUFFERED=1";
    appCommand = "/usr/bin/python3 multiCameraServer.py";
  } else if (appType == "upload-java") {
    appCommand =
        "env LD_LIBRARY_PATH=/usr/local/frc/lib java -jar uploaded.jar";
  } else if (appType == "upload-cpp") {
    appCommand = "./uploaded";
  } else if (appType == "upload-python") {
    appEnv = "export PYTHONUNBUFFERED=1";
    appCommand = "/usr/bin/python3 uploaded.py";
  } else if (appType == "custom") {
    return;
  } else {
    onFail(fmt::format("unrecognized application type '{}'", appType));
    return;
  }

  {
    // write file
    std::error_code ec;
    wpi::raw_fd_ostream os(EXEC_HOME "/runCamera", ec, fs::F_Text);
    if (ec) {
      onFail("could not write " EXEC_HOME "/runCamera");
      return;
    }
    wpi::print(os, "#!/bin/sh\n");
    wpi::print(os, "{} {}\n", TYPE_TAG, appType);
    wpi::print(os, "echo \"Waiting 5 seconds...\"\n");
    wpi::print(os, "sleep 5\n");
    if (!appDir.empty()) wpi::print(os, "cd {}\n", appDir);
    if (!appEnv.empty()) wpi::print(os, "{}\n", appEnv);
    wpi::print(os, "exec {}\n", appCommand);
  }

  // terminate vision process so it reloads
  VisionStatus::GetInstance()->Terminate(onFail);

  UpdateStatus();
}

void Application::FinishUpload(std::string_view appType, UploadHelper& helper,
                               std::function<void(std::string_view)> onFail) {
  std::string_view filename;
  if (appType == "upload-java") {
    filename = "/uploaded.jar";
  } else if (appType == "upload-cpp") {
    filename = "/uploaded";
  } else if (appType == "upload-python") {
    filename = "/uploaded.py";
  } else {
    onFail(fmt::format("cannot upload application type '{}'", appType));
    helper.Close();
    return;
  }

  int fd = helper.GetFD();

  // change ownership
  if (fchown(fd, APP_UID, APP_GID) == -1) {
    fmt::print(stderr, "could not change app ownership: {}\n",
               std::strerror(errno));
  }

  // set file to be executable
  if (fchmod(fd, 0775) == -1) {
    fmt::print(stderr, "could not change app permissions: {}\n",
               std::strerror(errno));
  }

  // close temporary file
  helper.Close();

  auto pathname = fmt::format("{}{}", EXEC_HOME, filename);

  // remove old file (need to do this as we can't overwrite a running exe)
  if (unlink(pathname.c_str()) == -1) {
    fmt::print(stderr, "could not remove app executable: {}\n",
               std::strerror(errno));
  }

  // rename temporary file to new file
  if (rename(helper.GetFilename(), pathname.c_str()) == -1) {
    fmt::print(stderr, "could not rename to app executable: {}\n",
               std::strerror(errno));
  }

  // terminate vision process so it reloads
  VisionStatus::GetInstance()->Terminate(onFail);
}

void Application::UpdateStatus() { status(GetStatusJson()); }

wpi::json Application::GetStatusJson() {
  wpi::json j = {{"type", "applicationSettings"},
                 {"applicationType", "custom"}};

  std::error_code ec;
  wpi::raw_fd_istream is(EXEC_HOME "/runCamera", ec);
  if (ec) {
    fmt::print(stderr, "could not read {}/runCamera\n", EXEC_HOME);
    return j;
  }

  // scan file
  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (wpi::starts_with(line, TYPE_TAG)) {
      j["applicationType"] = wpi::trim(wpi::substr(line, strlen(TYPE_TAG)));
      break;
    }
  }

  return j;
}
