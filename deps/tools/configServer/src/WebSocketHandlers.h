// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#ifndef RPICONFIGSERVER_WEBSOCKETHANDLERS_H_
#define RPICONFIGSERVER_WEBSOCKETHANDLERS_H_

#include <stdint.h>

#include <span>
#include <string_view>

namespace wpi {
class WebSocket;
}  // namespace wpi

void InitWs(wpi::WebSocket& ws);
void ProcessWsText(wpi::WebSocket& ws, std::string_view msg);
void ProcessWsBinary(wpi::WebSocket& ws, std::span<const uint8_t> msg);

#endif  // RPICONFIGSERVER_WEBSOCKETHANDLERS_H_
