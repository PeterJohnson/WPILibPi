// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "SystemStatus.h"

#include <string_view>

#include <wpi/SmallString.h>
#include <wpi/SmallVector.h>
#include <wpi/StringExtras.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>

std::shared_ptr<SystemStatus> SystemStatus::GetInstance() {
  static auto sysStatus = std::make_shared<SystemStatus>(private_init{});
  return sysStatus;
}

void SystemStatus::UpdateAll() {
  UpdateMemory();
  UpdateCpu();
  UpdateNetwork();
  UpdateTemp();
  status(GetStatusJson());
  writable(GetWritable());
}

wpi::json SystemStatus::GetStatusJson() {
  wpi::json j = {{"type", "systemStatus"}};

  size_t qty;

  // memory
  {
    uint64_t first;
    if (m_memoryFree.GetFirstLast(&first, nullptr, &qty)) {
      j["systemMemoryFree1s"] = first / 1000;
      if (qty >= 5)
        j["systemMemoryFree5s"] = m_memoryFree.GetTotal() / qty / 1000;
    }
    if (m_memoryAvail.GetFirstLast(&first, nullptr, &qty)) {
      j["systemMemoryAvail1s"] = first / 1000;
      if (qty >= 5)
        j["systemMemoryAvail5s"] = m_memoryAvail.GetTotal() / qty / 1000;
    }
  }

  // cpu
  {
    CpuData first, last;
    if (m_cpu.GetFirstLast(&first, &last, nullptr, 2)) {
      uint64_t deltaTotal = last.total - first.total;
      if (deltaTotal != 0) {
        j["systemCpuUser1s"] =
            (last.user + last.nice - first.user - first.nice) * 100 /
            deltaTotal;
        j["systemCpuSystem1s"] =
            (last.system - first.system) * 100 / deltaTotal;
        j["systemCpuIdle1s"] = (last.idle - first.idle) * 100 / deltaTotal;
      }
    }
    if (m_cpu.GetFirstLast(&first, &last, nullptr, 6)) {
      uint64_t deltaTotal = last.total - first.total;
      if (deltaTotal != 0) {
        j["systemCpuUser5s"] =
            (last.user + last.nice - first.user - first.nice) * 100 /
            deltaTotal;
        j["systemCpuSystem5s"] =
            (last.system - first.system) * 100 / deltaTotal;
        j["systemCpuIdle5s"] = (last.idle - first.idle) * 100 / deltaTotal;
      }
    }
  }

  // network
  {
    NetworkData first, last;
    if (m_network.GetFirstLast(&first, &last, nullptr, 2)) {
      j["systemNetwork1s"] = (last.recvBytes + last.xmitBytes -
                              first.recvBytes - first.xmitBytes) *
                             8 / 1000;
    }
    if (m_network.GetFirstLast(&first, &last, nullptr, 6)) {
      j["systemNetwork5s"] = (last.recvBytes + last.xmitBytes -
                              first.recvBytes - first.xmitBytes) *
                             8 / 5000;
    }
  }

  // temperature
  {
    uint64_t first;
    if (m_temp.GetFirstLast(&first, nullptr, &qty)) {
      j["systemCpuTemp1s"] = first / 1000;
      if (qty >= 5) j["systemCpuTemp5s"] = m_temp.GetTotal() / qty / 1000;
    }
  }

  return j;
}

bool SystemStatus::GetWritable() {
  std::error_code ec;
  wpi::raw_fd_istream is("/proc/mounts", ec);
  if (ec) return false;
  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (line.empty()) break;

    wpi::SmallVector<std::string_view, 8> strs;
    wpi::split(line, strs, ' ', -1, false);
    if (strs.size() < 4) continue;

    if (strs[1] == "/") return wpi::contains(strs[3], "rw");
  }
  return false;
}

void SystemStatus::UpdateMemory() {
  std::error_code ec;
  wpi::raw_fd_istream is("/proc/meminfo", ec);
  if (ec) return;
  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (line.empty()) break;

    std::string_view name, amtStr;
    std::tie(name, amtStr) = wpi::split(line, ':');

    amtStr = wpi::trim(amtStr);
    if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
      if (name == "MemFree") {
        m_memoryFree.Add(*val);
      } else if (name == "MemAvailable") {
        m_memoryAvail.Add(*val);
      }
    }
  }
}

void SystemStatus::UpdateCpu() {
  std::error_code ec;
  wpi::raw_fd_istream is("/proc/stat", ec);
  if (ec) return;
  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (line.empty()) break;

    std::string_view name, amtStr;
    std::tie(name, amtStr) = wpi::split(line, ' ');
    if (name == "cpu") {
      CpuData data;

      // individual values we care about
      amtStr = wpi::ltrim(amtStr);
      if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
        data.user = *val;
      } else {
        break;
      }
      amtStr = wpi::ltrim(amtStr);
      if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
        data.nice = *val;
      } else {
        break;
      }
      amtStr = wpi::ltrim(amtStr);
      if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
        data.system = *val;
      } else {
        break;
      }
      amtStr = wpi::ltrim(amtStr);
      if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
        data.idle = *val;
      } else {
        break;
      }

      // compute total
      data.total = data.user + data.nice + data.system + data.idle;
      for (;;) {
        amtStr = wpi::ltrim(amtStr);
        if (auto val = wpi::consume_integer<uint64_t>(&amtStr, 10)) {
          data.total += *val;
        } else {
          break;
        }
      }

      m_cpu.Add(data);
      break;
    }
  }
}

void SystemStatus::UpdateNetwork() {
  std::error_code ec;
  wpi::raw_fd_istream is("/proc/net/dev", ec);
  if (ec) return;

  NetworkData data;

  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (line.empty()) break;

    std::string_view name, amtStr;
    std::tie(name, amtStr) = wpi::split(line, ':');
    name = wpi::trim(name);
    if (name.empty() || name == "lo") continue;

    wpi::SmallVector<std::string_view, 20> amtStrs;
    wpi::split(amtStr, amtStrs, ' ', -1, false);
    if (amtStrs.size() < 16) continue;

    // receive bytes
    if (auto val = wpi::parse_integer<uint64_t>(amtStrs[0], 10)) {
      data.recvBytes += *val;
    }

    // transmit bytes
    if (auto val = wpi::parse_integer<uint64_t>(amtStrs[8], 10)) {
      data.xmitBytes += *val;
    }
  }

  m_network.Add(data);
}

void SystemStatus::UpdateTemp() {
  std::error_code ec;
  wpi::raw_fd_istream is("/sys/class/thermal/thermal_zone0/temp", ec);
  if (ec) return;
  wpi::SmallString<256> lineBuf;
  while (!is.has_error()) {
    std::string_view line = wpi::trim(is.getline(lineBuf, 256));
    if (line.empty()) break;

    if (auto amt = wpi::parse_integer<uint64_t>(line, 10)) {
      m_temp.Add(*amt);
    }
  }
}
