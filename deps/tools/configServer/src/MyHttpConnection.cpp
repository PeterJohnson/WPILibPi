// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "MyHttpConnection.h"

#include <unistd.h>
#include <uv.h>

#include <fmt/format.h>
#include <wpi/fs.h>
#include <wpi/SmallVector.h>
#include <wpi/StringExtras.h>
#include <wpi/raw_ostream.h>
#include <wpinet/UrlParser.h>
#include <wpinet/raw_uv_ostream.h>
#include <wpinet/uv/Request.h>

#include "WebSocketHandlers.h"

#define ZIPS_DIR "/home/pi/zips"

namespace uv = wpi::uv;

// static resources
namespace wpi {
std::string_view GetResource_bootstrap_4_1_min_js_gz();
std::string_view GetResource_coreui_2_1_min_css_gz();
std::string_view GetResource_coreui_2_1_min_js_gz();
std::string_view GetResource_feather_4_8_min_js_gz();
std::string_view GetResource_jquery_3_3_slim_min_js_gz();
std::string_view GetResource_popper_1_14_min_js_gz();
std::string_view GetResource_wpilib_128_png();
}  // namespace wpi
std::string_view GetResource_frcvision_css();
std::string_view GetResource_frcvision_js();
std::string_view GetResource_index_html();
std::string_view GetResource_romi_ext_io_png();

MyHttpConnection::MyHttpConnection(std::shared_ptr<wpi::uv::Stream> stream)
    : HttpServerConnection(stream), m_websocketHelper(m_request) {
  // Handle upgrade event
  m_websocketHelper.upgrade.connect([this] {
    //fmt::print(stderr, "got websocket upgrade\n");
    // Disconnect HttpServerConnection header reader
    m_dataConn.disconnect();
    m_messageCompleteConn.disconnect();

    // Accepting the stream may destroy this (as it replaces the stream user
    // data), so grab a shared pointer first.
    auto self = shared_from_this();

    // Accept the upgrade
    auto ws = m_websocketHelper.Accept(m_stream, "frcvision");

    // Connect the websocket open event to our connected event.
    // Pass self to delay destruction until this callback happens
    ws->open.connect_extended([self, s = ws.get()](auto conn, std::string_view) {
      fmt::print(stderr, "websocket connected\n");
      InitWs(*s);
      conn.disconnect();  // one-shot
    });
    ws->text.connect([s = ws.get()](std::string_view msg, bool) {
      ProcessWsText(*s, msg);
    });
    ws->binary.connect([s = ws.get()](std::span<const uint8_t> msg, bool) {
      ProcessWsBinary(*s, msg);
    });
  });
}

class SendfileReq : public uv::RequestImpl<SendfileReq, uv_fs_t> {
 public:
  SendfileReq(uv_file out, uv_file in, int64_t inOffset, size_t len)
      : m_out(out), m_in(in), m_inOffset(inOffset), m_len(len) {
    error = [this](uv::Error err) { GetLoop().error(err); };
  }

  uv::Loop& GetLoop() const {
    return *static_cast<uv::Loop*>(GetRaw()->loop->data);
  }

  int Send(uv::Loop& loop) {
    int err = uv_fs_sendfile(loop.GetRaw(), GetRaw(), m_out, m_in, m_inOffset,
                             m_len, [](uv_fs_t* req) {
                               auto& h = *static_cast<SendfileReq*>(req->data);
                               if (req->result < 0) {
                                 h.ReportError(req->result);
                                 h.complete();
                                 h.Release();
                                 return;
                               }

                               h.m_inOffset += req->result;
                               h.m_len -= req->result;
                               if (h.m_len == 0) {
                                 // done
                                 h.complete();
                                 h.Release();  // this is always a one-shot
                                 return;
                               }

                               // need to send more
                               h.Send(h.GetLoop());
                             });
    if (err < 0) {
      ReportError(err);
      complete();
    }
    return err;
  }

  wpi::sig::Signal<> complete;

 private:
  uv_file m_out;
  uv_file m_in;
  int64_t m_inOffset;
  size_t m_len;
};

void Sendfile(uv::Loop& loop, uv_file out, uv_file in, int64_t inOffset,
              size_t len, std::function<void()> complete) {
  auto req = std::make_shared<SendfileReq>(out, in, inOffset, len);
  if (complete) req->complete.connect(complete);
  int err = req->Send(loop);
  if (err >= 0) req->Keep();
}

void MyHttpConnection::SendFileResponse(int code, std::string_view codeText,
                                        std::string_view contentType,
                                        std::string_view filename,
                                        std::string_view extraHeader) {
  // open file
  std::error_code ec;
  auto infile = fs::OpenFileForRead(filename, ec);
  if (ec) {
    SendError(404);
    return;
  }
  int infd = fs::FileToFd(infile, ec, fs::OF_None);
  if (ec) {
    fs::CloseFile(infile);
    SendError(404);
    return;
  }

  // get status (to get file size)
  auto size = fs::file_size(filename, ec);
  if (ec) {
    SendError(404);
    ::close(infd);
    return;
  }

  uv_os_fd_t outfd;
  int err = uv_fileno(m_stream.GetRawHandle(), &outfd);
  if (err < 0) {
    m_stream.GetLoopRef().ReportError(err);
    SendError(404);
    ::close(infd);
    return;
  }

  wpi::SmallVector<uv::Buffer, 4> toSend;
  wpi::raw_uv_ostream os{toSend, 4096};
  BuildHeader(os, code, codeText, contentType, size, extraHeader);
  SendData(os.bufs(), false);

  // close after write completes if we aren't keeping alive
  // since we're using sendfile, set socket to blocking
  m_stream.SetBlocking(true);
  Sendfile(m_stream.GetLoopRef(), outfd, infd, 0, size,
           [ infd, closeAfter = !m_keepAlive, stream = &m_stream ] {
             ::close(infd);
             if (closeAfter)
               stream->Close();
             else
               stream->SetBlocking(false);
           });
}

void MyHttpConnection::ProcessRequest() {
  //fmt::print(stderr, "HTTP request: '{}'\n", m_request.GetUrl());
  wpi::UrlParser url{m_request.GetUrl(),
                     m_request.GetMethod() == wpi::HTTP_CONNECT};
  if (!url.IsValid()) {
    // failed to parse URL
    SendError(400);
    return;
  }

  std::string_view path;
  if (url.HasPath()) path = url.GetPath();
  //fmt::print(stderr, "path: \"{}\"\n", path);

  std::string_view query;
  if (url.HasQuery()) query = url.GetQuery();
  //fmt::print(stderr, "query: \"{}\"\n", query);

  const bool isGET = m_request.GetMethod() == wpi::HTTP_GET;
  if (isGET && (path == "/" || path == "/index.html")) {
    SendStaticResponse(200, "OK", "text/html", GetResource_index_html(), false);
  } else if (isGET && path == "/frcvision.css") {
    SendStaticResponse(200, "OK", "text/css", GetResource_frcvision_css(),
                       false);
  } else if (isGET && path == "/frcvision.js") {
    SendStaticResponse(200, "OK", "text/javascript", GetResource_frcvision_js(),
                       false);
  } else if (isGET && path == "/bootstrap.min.js") {
    SendStaticResponse(200, "OK", "text/javascript",
                       wpi::GetResource_bootstrap_4_1_min_js_gz(), true);
  } else if (isGET && path == "/coreui.min.css") {
    SendStaticResponse(200, "OK", "text/css",
                       wpi::GetResource_coreui_2_1_min_css_gz(), true);
  } else if (isGET && path == "/coreui.min.js") {
    SendStaticResponse(200, "OK", "text/javascript",
                       wpi::GetResource_coreui_2_1_min_js_gz(), true);
  } else if (isGET && path == "/feather.min.js") {
    SendStaticResponse(200, "OK", "text/javascript",
                       wpi::GetResource_feather_4_8_min_js_gz(), true);
  } else if (isGET && path == "/jquery-3.3.1.slim.min.js") {
    SendStaticResponse(200, "OK", "text/javascript",
                       wpi::GetResource_jquery_3_3_slim_min_js_gz(), true);
  } else if (isGET && path == "/popper.min.js") {
    SendStaticResponse(200, "OK", "text/javascript",
                       wpi::GetResource_popper_1_14_min_js_gz(), true);
  } else if (isGET && path == "/wpilib.png") {
    SendStaticResponse(200, "OK", "image/png",
                       wpi::GetResource_wpilib_128_png(), false);
  } else if (isGET && path == "/romi_ext_io.png") {
    SendStaticResponse(200, "OK", "image/png",
                       GetResource_romi_ext_io_png(), false);
  } else if (isGET && wpi::starts_with(path, '/') &&
             wpi::ends_with(path, ".zip") && !wpi::contains(path, "..")) {
    SendFileResponse(200, "OK", "application/zip",
                     fmt::format("{}{}", ZIPS_DIR, path));
  } else {
    SendError(404, "Resource not found");
  }
}
