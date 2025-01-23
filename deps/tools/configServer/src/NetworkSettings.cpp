// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "NetworkSettings.h"

#include <unistd.h>

#include <bit>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <wpi/MathExtras.h>
#include <wpi/SmallString.h>
#include <wpi/StringExtras.h>
#include <wpi/fmt/raw_ostream.h>
#include <wpi/fs.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>
#include <wpinet/uv/Process.h>
#include <wpinet/uv/util.h>

namespace uv = wpi::uv;

extern bool romi;

#define GEN_MARKER "###### BELOW THIS LINE EDITED BY RPICONFIGSERVER ######"

/*
 Format of generated portion for static:

   interface eth0
   static ip_address=<networkAddress>/<networkMask as CIDR>
   static routers=<networkGateway>
   static domain_name_servers=<networkDNS>

 For static fallback:

   profile static_eth0
   static ip_address=<networkAddress>/<networkMask as CIDR>
   static routers=<networkGateway>
   static domain_name_servers=<networkDNS>
   interface eth0
   fallback static_eth0
 */

std::string_view CidrToNetmask(unsigned int cidr,
                               wpi::SmallVectorImpl<char>& buf) {
  in_addr addr = { htonl(wpi::maskLeadingOnes<uint32_t>(cidr)) };
  wpi::uv::AddrToName(addr, &buf);
  return std::string_view(buf.data(), buf.size());
}

bool NetmaskToCidr(std::string_view netmask, unsigned int* cidr) {
  in_addr addr;
  if (wpi::uv::NameToAddr(netmask, &addr) != 0) return false;
  uint32_t hostAddr = ntohl(addr.s_addr);
  auto leadingOnes = std::countl_one(hostAddr);
  auto trailingZeros = std::countr_zero(hostAddr);
  if (leadingOnes + trailingZeros != 32) return false;
  *cidr = leadingOnes;
  return true;
}

std::shared_ptr<NetworkSettings> NetworkSettings::GetInstance() {
  static auto inst = std::make_shared<NetworkSettings>(private_init{});
  return inst;
}

static std::string BuildDhcpcdSetting(
    std::string_view iface, NetworkSettings::Mode mode,
    std::string_view address, std::string_view mask, std::string_view gateway,
    std::string_view dns, std::function<void(std::string_view)> onFail) {
  // validate and sanitize inputs
  wpi::SmallString<32> addressOut;
  unsigned int cidr;
  wpi::SmallString<32> gatewayOut;
  wpi::SmallString<128> dnsOut;

  // address
  in_addr addressAddr;
  if (wpi::uv::NameToAddr(address, &addressAddr) != 0) {
    onFail(fmt::format("invalid address '{}'", address));
    return {};
  }
  wpi::uv::AddrToName(addressAddr, &addressOut);

  // mask
  if (!NetmaskToCidr(mask, &cidr)) {
    onFail(fmt::format("invalid netmask '{}'", mask));
    return {};
  }

  // gateway (may be blank)
  in_addr gatewayAddr;
  if (wpi::uv::NameToAddr(gateway, &gatewayAddr) == 0)
    wpi::uv::AddrToName(gatewayAddr, &gatewayOut);

  // dns
  wpi::SmallVector<std::string_view, 4> dnsStrs;
  wpi::SmallString<32> oneDnsOut;
  bool first = true;
  wpi::split(dns, dnsStrs, ' ', -1, false);
  for (auto dnsStr : dnsStrs) {
    in_addr dnsAddr;
    if (wpi::uv::NameToAddr(dnsStr, &dnsAddr) != 0) {
      onFail(fmt::format("invalid DNS address '{}'", dnsStr));
      return {};
    }
    wpi::uv::AddrToName(dnsAddr, &oneDnsOut);
    if (!first) dnsOut += ' ';
    first = false;
    dnsOut += oneDnsOut;
  }

  std::string rv;
  wpi::raw_string_ostream os{rv};
  // write generated config
  switch (mode) {
    case NetworkSettings::kDhcp:
      wpi::print(os, "\n");  // nothing required
      break;
    case NetworkSettings::kStatic:
      wpi::print(os, "interface {}\n", iface);
      wpi::print(os, "static ip_address={}/{}\n", addressOut.str(), cidr);
      if (!gatewayOut.empty())
        wpi::print(os, "static routers={}\n", gatewayOut.str());
      if (!dnsOut.empty())
        wpi::print(os, "static domain_name_servers={}\n", dnsOut.str());
      break;
    case NetworkSettings::kDhcpStatic:
      wpi::print(os, "profile static_{}\n", iface);
      wpi::print(os, "static ip_address={}/{}\n", addressOut.str(), cidr);
      if (!gatewayOut.empty())
        wpi::print(os, "static routers={}\n", gatewayOut.str());
      if (!dnsOut.empty())
        wpi::print(os, "static domain_name_servers={}\n", dnsOut.str());
      wpi::print(os, "interface {}\n", iface);
      wpi::print(os, "fallback static_{}\n", iface);
      break;
  }
  return os.str();
}

void NetworkSettings::Set(Mode mode, std::string_view address,
                          std::string_view mask, std::string_view gateway,
                          std::string_view dns, WifiMode wifiAPMode,
                          int wifiChannel, std::string_view wifiSsid,
                          std::string_view wifiWpa2, Mode wifiMode,
                          std::string_view wifiAddress,
                          std::string_view wifiMask,
                          std::string_view wifiGateway,
                          std::string_view wifiDns,
                          std::function<void(std::string_view)> onFail) {
  // sanity check access point settings
  if (wifiAPMode == kAccessPoint) {
    if (wifiSsid.empty()) {
      onFail("must set SSID for access point");
      return;
    }
    if (wifiMode != kStatic || wifiAddress.empty() || wifiMask.empty()) {
      onFail("must set static address and netmask for access point");
      return;
    }
    if (!wifiWpa2.empty() && (wifiWpa2.size() < 8 || wifiWpa2.size() > 63)) {
      onFail("passphrase must be between 8 and 63 characters");
      return;
    }
  }

  //
  // dhcpcd configuration
  //
  {
    std::string eth0 =
        BuildDhcpcdSetting("eth0", mode, address, mask, gateway, dns, onFail);
    if (eth0.empty()) return;
    std::string wlan0;
    if (romi) {
      wlan0 = BuildDhcpcdSetting("wlan0", wifiMode, wifiAddress, wifiMask,
                                 wifiGateway, wifiDns, onFail);
      if (wlan0.empty()) return;
    }

    // read file (up to but not including the marker)
    std::vector<std::string> lines;
    std::error_code ec;
    {
      wpi::raw_fd_istream is(DHCPCD_CONF, ec);
      if (ec) {
        onFail("could not read " DHCPCD_CONF);
        return;
      }

      wpi::SmallString<256> lineBuf;
      while (!is.has_error()) {
        std::string_view line = wpi::trim(is.getline(lineBuf, 256));
        if (line == GEN_MARKER) break;
        lines.emplace_back(std::string(line));
      }
    }

    // write file
    {
      // write original lines
      wpi::raw_fd_ostream os(DHCPCD_CONF, ec, fs::F_Text);
      if (ec) {
        onFail("could not write " DHCPCD_CONF);
        return;
      }
      for (auto&& line : lines) wpi::print(os, "{}\n", line);

      // write marker
      wpi::print(os, "{}\n", GEN_MARKER);

      // write generated config
      wpi::print(os, "{}\n", eth0);
      wpi::print(os, "{}", wlan0);
      if (wifiAPMode == kAccessPoint) {
        wpi::print(os, "nohook wpa_supplicant\n");
      }
    }
  }

  //
  // dnsmasq configuration
  //
  if (wifiAPMode == kAccessPoint) {
    // take the 3 parts of IP range
    in_addr addressAddr;
    if (wpi::uv::NameToAddr(wifiAddress, &addressAddr) != 0) {
      onFail(fmt::format("invalid address '{}'", wifiAddress));
      return;
    }
    wpi::SmallString<32> addressOut;
    wpi::uv::AddrToName(addressAddr, &addressOut);
    std::string_view addr3part = wpi::rsplit(addressOut, '.').first;

    std::error_code ec;
    wpi::raw_fd_ostream os(DNSMASQ_CONF, ec, fs::F_Text);
    if (ec) {
      onFail("could not write " DNSMASQ_CONF);
      return;
    }
    wpi::print(os, "interface=wlan0\n");
    wpi::print(os, "dhcp-range={}.100,{}.200,{},5m\n", addr3part, addr3part,
               wifiMask);
  } else {
    // remove dnsmasq config file
    unlink(DNSMASQ_CONF);
  }

  //
  // hostapd configuration
  //
  if (wifiAPMode == kAccessPoint) {
    std::error_code ec;
    wpi::raw_fd_ostream os(HOSTAPD_CONF, ec, fs::F_Text);
    if (ec) {
      onFail("could not write " HOSTAPD_CONF);
      return;
    }
    wpi::print(os, "interface=wlan0\n");
    wpi::print(os, "hw_mode=g\n");
    wpi::print(os, "channel={}\n", wifiChannel);
    wpi::print(os, "wmm_enabled=0\n");
    wpi::print(os, "macaddr_acl=0\n");
    wpi::print(os, "auth_algs=1\n");
    wpi::print(os, "ignore_broadcast_ssid=0\n");
    wpi::print(os, "ssid={}\n", wifiSsid);
    if (!wifiWpa2.empty()) {
      wpi::print(os, "wpa=2\n");
      wpi::print(os, "wpa_key_mgmt=WPA-PSK\n");
      wpi::print(os, "wpa_pairwise=TKIP\n");
      wpi::print(os, "rsn_pairwise=CCMP\n");
      wpi::print(os, "wpa_passphrase={}\n", wifiWpa2);
    }
  } else {
    // remove hostapd config file
    unlink(HOSTAPD_CONF);
  }

  //
  // wpa supplicant configuration
  //
  {
    // read file (up to but not including the marker)
    std::vector<std::string> lines;
    std::error_code ec;
    {
      wpi::raw_fd_istream is(WPA_SUPPLICANT_CONF, ec);
      if (ec) {
        onFail("could not read " WPA_SUPPLICANT_CONF);
        return;
      }

      wpi::SmallString<256> lineBuf;
      while (!is.has_error()) {
        std::string_view line = wpi::trim(is.getline(lineBuf, 256));
        if (line == GEN_MARKER) break;
        lines.emplace_back(std::string(line));
      }
    }

    // write file
    {
      // write original lines
      wpi::raw_fd_ostream os(WPA_SUPPLICANT_CONF, ec, fs::F_Text);
      if (ec) {
        onFail("could not write " WPA_SUPPLICANT_CONF);
        return;
      }
      for (auto&& line : lines) wpi::print(os, "{}\n", line);

      // write marker
      wpi::print(os, "{}\n", GEN_MARKER);

      // write generated config
      wpi::print(os, "network={\n");
      wpi::print(os, "    ssid=\"{}\"\n", wifiSsid);
      if (!wifiWpa2.empty()) {
        wpi::print(os, "    psk=\"{}\"\n", wifiWpa2);
      } else {
        wpi::print(os, "    key_mgmt=NONE\n");
      }
      wpi::print(os, "}\n");
    }
  }

  // start or stop hostapd
  if (wifiAPMode == kAccessPoint) {
    if (auto proc =
            uv::Process::Spawn(m_loop, "/usr/bin/systemctl",
                               "/usr/bin/systemctl", "unmask", "hostapd")) {
      proc->exited.connect([p = proc.get(), loop = m_loop](int64_t, int) {
        p->Close();
        if (auto proc = uv::Process::Spawn(loop, "/usr/bin/systemctl",
                                           "/usr/bin/systemctl", "restart",
                                           "hostapd")) {
          proc->exited.connect([p = proc.get()](int64_t, int) { p->Close(); });
        }
      });
    }
  } else {
    if (auto proc = uv::Process::Spawn(m_loop, "/usr/bin/systemctl",
                                       "/usr/bin/systemctl", "--now", "mask",
                                       "hostapd")) {
      proc->exited.connect([p = proc.get()](int64_t, int) { p->Close(); });
    }
  }

  // tell dnsmasq to restart
  if (auto proc =
          uv::Process::Spawn(m_loop, "/usr/sbin/service", "/usr/sbin/service",
                             "dnsmasq", "restart")) {
    proc->exited.connect([p = proc.get()](int64_t, int) { p->Close(); });
  }

  // tell dhcpcd to reload config
  if (auto proc =
          uv::Process::Spawn(m_loop, "/sbin/dhcpcd", "/sbin/dhcpcd", "-n")) {
    proc->exited.connect([p = proc.get()](int64_t, int) { p->Close(); });
  }

  // tell wpa to reconfigure
  if (auto proc = uv::Process::Spawn(m_loop, "/sbin/wpa_cli", "/sbin/wpa_cli",
                                     "-i", "wlan0", "reconfigure")) {
    proc->exited.connect([p = proc.get()](int64_t, int) { p->Close(); });
  }

  UpdateStatus();
}

void NetworkSettings::UpdateStatus() { status(GetStatusJson()); }

wpi::json NetworkSettings::GetStatusJson() {
  wpi::json j = {{"type", "networkSettings"},
                 {"networkApproach", "dhcp"},
                 {"wifiNetworkApproach", "dhcp"}};

  //
  // dhcpcd configuration
  //
  {
    std::error_code ec;
    wpi::raw_fd_istream is(DHCPCD_CONF, ec);
    if (ec) {
      fmt::print(stderr, "could not read {}\n", DHCPCD_CONF);
      return wpi::json();
    }

    wpi::SmallString<256> lineBuf;
    bool foundMarker = false;
    bool wlan = false;
    while (!is.has_error()) {
      std::string_view line = wpi::trim(is.getline(lineBuf, 256));
      if (line == GEN_MARKER) foundMarker = true;
      if (!foundMarker) continue;
      if (line.empty()) continue;
      if (wpi::contains(line, "wlan0")) wlan = true;
      if (wpi::starts_with(line, "static ip_address")) {
        j[wlan ? "wifiNetworkApproach" : "networkApproach"] = "static";

        std::string_view value = wpi::trim(wpi::split(line, '=').second);
        std::string_view cidrStr;
        std::tie(j[wlan ? "wifiNetworkAddress" : "networkAddress"], cidrStr) =
            wpi::split(value, '/');

        if (auto cidrInt = wpi::parse_integer<unsigned int>(cidrStr, 10)) {
          wpi::SmallString<64> netmaskBuf;
          j[wlan ? "wifiNetworkMask" : "networkMask"] =
              CidrToNetmask(*cidrInt, netmaskBuf);
        }
      } else if (wpi::starts_with(line, "static routers")) {
        j[wlan ? "wifiNetworkGateway" : "networkGateway"] =
            wpi::trim(wpi::split(line, '=').second);
      } else if (wpi::starts_with(line, "static domain_name_servers")) {
        j[wlan ? "wifiNetworkDNS" : "networkDNS"] =
            wpi::trim(wpi::split(line, '=').second);
      } else if (wpi::starts_with(line, "fallback")) {
        j[wlan ? "wifiNetworkApproach" : "networkApproach"] = "dhcp-fallback";
      }
    }
  }

  //
  // hostapd configuration
  //
  {
    std::error_code ec;
    wpi::raw_fd_istream is(HOSTAPD_CONF, ec);
    if (ec) {
      j["wifiMode"] = "bridge";
      j["wifiChannel"] = "";
    } else {
      j["wifiMode"] = "access-point";
      wpi::SmallString<256> lineBuf;
      while (!is.has_error()) {
        std::string_view line = wpi::trim(is.getline(lineBuf, 256));
        if (wpi::starts_with(line, "channel=")) {
          j["wifiChannel"] = wpi::drop_front(line, 8);
        }
      }
    }
  }

  //
  // wpa supplicant configuration
  //
  {
    std::error_code ec;
    wpi::raw_fd_istream is(WPA_SUPPLICANT_CONF, ec);
    if (ec) {
      fmt::print(stderr, "could not read {}\n", WPA_SUPPLICANT_CONF);
      return j;
    }

    wpi::SmallString<256> lineBuf;
    bool foundMarker = false;
    while (!is.has_error()) {
      std::string_view line = wpi::trim(is.getline(lineBuf, 256));
      if (line == GEN_MARKER) foundMarker = true;
      if (!foundMarker) continue;
      if (line.empty()) continue;
      if (wpi::contains(line, "ssid")) {
        j["wifiSsid"] =
            wpi::drop_back(wpi::drop_front(wpi::trim(wpi::split(line,
                                                                '=').second)));
      } else if (wpi::contains(line, "psk")) {
        j["wifiWpa2"] =
            wpi::drop_back(wpi::drop_front(wpi::trim(wpi::split(line,
                                                                '=').second)));
      } else if (wpi::contains(line, "key_mgmt")) {
        j["wifiWpa2"] = "";
      }
    }
  }

  return j;
}
