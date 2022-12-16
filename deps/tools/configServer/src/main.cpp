// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include <chrono>
#include <span>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <wpi/timestamp.h>
#include <wpi/MathExtras.h>
#include <wpinet/raw_uv_ostream.h>
#include <wpinet/uv/Loop.h>
#include <wpinet/uv/Process.h>
#include <wpinet/uv/Tcp.h>
#include <wpinet/uv/Timer.h>
#include <wpinet/uv/Udp.h>

#include "MyHttpConnection.h"
#include "NetworkSettings.h"
#include "RomiStatus.h"
#include "SystemStatus.h"
#include "VisionStatus.h"

namespace uv = wpi::uv;

bool romi = false;
static uint64_t startTime = wpi::Now();

int main(int argc, char* argv[]) {
  int port = 80;
  if (argc >= 2 && std::string_view{argv[1]} == "--romi") {
    --argc;
    ++argv;
    romi = true;
  }
  if (argc == 2) port = std::atoi(argv[1]);

  uv::Process::DisableStdioInheritance();

  SystemStatus::GetInstance();

  auto loop = uv::Loop::Create();

  NetworkSettings::GetInstance()->SetLoop(loop);
  if (romi) RomiStatus::GetInstance()->SetLoop(loop);
  VisionStatus::GetInstance()->SetLoop(loop);

  loop->error.connect(
      [](uv::Error err) { fmt::print(stderr, "uv ERROR: {}\n", err.str()); });

  auto tcp = uv::Tcp::Create(loop);

  // bind to listen address and port
  tcp->Bind("", port);

  // when we get a connection, accept it and start reading
  tcp->connection.connect([srv = tcp.get()] {
    auto tcp = srv->Accept();
    if (!tcp) return;
    // fmt::print(stderr, "Got a connection\n");

    // Close on error
    tcp->error.connect([s = tcp.get()](wpi::uv::Error err) {
      fmt::print(stderr, "stream error: {}\n", err.str());
      s->Close();
    });

    auto conn = std::make_shared<MyHttpConnection>(tcp);
    tcp->SetData(conn);
  });

  // start listening for incoming connections
  tcp->Listen();

  fmt::print(stderr, "Listening on port {}\n", port);

  // start timer to collect system and vision status
  auto timer = uv::Timer::Create(loop);
  timer->Start(std::chrono::seconds(1), std::chrono::seconds(1));
  timer->timeout.connect([&loop] {
    SystemStatus::GetInstance()->UpdateAll();
    VisionStatus::GetInstance()->UpdateStatus();
    if (romi) {
      RomiStatus::GetInstance()->UpdateStatus();
    }
  });

  // listen on port 6666 for console logging
  auto udpCon = uv::Udp::Create(loop);
  udpCon->Bind("127.0.0.1", 6666, UV_UDP_REUSEADDR);
  udpCon->StartRecv();
  udpCon->received.connect(
      [](uv::Buffer& buf, size_t len, const sockaddr&, unsigned) {
        VisionStatus::GetInstance()->ConsoleLog(buf, len);
      });

  // listen on port 7777 for romi console logging
  if (romi) {
    auto udpCon = uv::Udp::Create(loop);
    udpCon->Bind("127.0.0.1", 7777, UV_UDP_REUSEADDR);
    udpCon->StartRecv();
    udpCon->received.connect(
        [](uv::Buffer& buf, size_t len, const sockaddr&, unsigned) {
          RomiStatus::GetInstance()->ConsoleLog(buf, len);
        });
  }

  // create riolog console port
  auto tcpCon = uv::Tcp::Create(loop);
  tcpCon->Bind("", 1740);

  // when we get a connection, accept it
  tcpCon->connection.connect([ srv = tcpCon.get(), udpCon ] {
    auto tcp = srv->Accept();
    if (!tcp) return;

    // close on error
    tcp->error.connect([s = tcp.get()](uv::Error err) { s->Close(); });

    // copy console log to it with headers
    udpCon->received.connect(
        [ tcpSeq = std::make_shared<uint16_t>(), tcpPtr = tcp.get() ](
            uv::Buffer & buf, size_t len, const sockaddr&, unsigned) {
          // build buffers
          wpi::SmallVector<uv::Buffer, 4> bufs;
          wpi::raw_uv_ostream out(bufs, 4096);

          // Header is 2 byte len, 1 byte type, 4 byte timestamp, 2 byte
          // sequence num
          uint32_t ts = wpi::FloatToBits((wpi::Now() - startTime) * 1.0e-6);
          uint16_t pktlen = len + 1 + 4 + 2;
          const uint8_t contents[] =
              {static_cast<uint8_t>((pktlen >> 8) & 0xff),
               static_cast<uint8_t>(pktlen & 0xff), 12,
               static_cast<uint8_t>((ts >> 24) & 0xff),
               static_cast<uint8_t>((ts >> 16) & 0xff),
               static_cast<uint8_t>((ts >> 8) & 0xff),
               static_cast<uint8_t>(ts & 0xff),
               static_cast<uint8_t>((*tcpSeq >> 8) & 0xff),
               static_cast<uint8_t>(*tcpSeq & 0xff)};
          out << std::span<const uint8_t>{contents};
          out << std::string_view{buf.base, len};
          (*tcpSeq)++;

          // send output
          tcpPtr->Write(bufs, [](auto bufs2, uv::Error) {
            for (auto buf : bufs2) buf.Deallocate();
          });
        },
        tcp);
  });

  // start listening for incoming connections
  tcpCon->Listen();

  // run loop
  loop->Run();
}
