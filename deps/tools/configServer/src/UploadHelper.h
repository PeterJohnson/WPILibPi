// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#ifndef RPICONFIGSERVER_UPLOADHELPER_H_
#define RPICONFIGSERVER_UPLOADHELPER_H_

#include <stdint.h>

#include <functional>
#include <span>
#include <string>
#include <string_view>

class UploadHelper {
 public:
  UploadHelper() = default;
  ~UploadHelper() { Close(); }

  UploadHelper(const UploadHelper&) = delete;
  UploadHelper& operator=(const UploadHelper&) = delete;

  UploadHelper(UploadHelper&& oth);
  UploadHelper& operator=(UploadHelper&& oth);

  explicit operator bool() const { return m_fd != -1; }

  const char* GetFilename() { return m_filename.c_str(); }
  int GetFD() const { return m_fd; }

  bool Open(std::string_view filename, bool text,
            std::function<void(std::string_view)> onFail);
  void Write(std::span<const uint8_t> contents);
  void Close();

 private:
  std::string m_filename;
  int m_fd = -1;
  bool m_text;
  bool m_hasEol;
};

#endif  // RPICONFIGSERVER_UPLOADHELPER_H_
