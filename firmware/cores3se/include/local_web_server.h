#pragma once
#include <WebServer.h>
#include <lwip/sockets.h>

// Arduino 2.0.17's request parser busy-waits for missing header delimiters.
// Admit only complete, bounded headers, without consuming bytes or blocking
// the HTTP task. The normal parser retains ownership of body/upload handling.
class LocalWebServer : public WebServer {
 public:
  explicit LocalWebServer(int port) : WebServer(port) {}
  void handleClient() override {
    if (_currentStatus == HC_NONE) {
      _currentClient = _server.available();
      if (!_currentClient) return;
      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }
    if (_currentStatus == HC_WAIT_READ) {
      char header[2049];
      const int bytes = recv(_currentClient.fd(), header, sizeof(header) - 1, MSG_PEEK | MSG_DONTWAIT);
      if (bytes <= 0) {
        if (millis() - _statusChange >= 1500) dropClient();
        return;
      }
      header[bytes] = 0;
      if (!strstr(header, "\r\n\r\n")) {
        if (bytes == sizeof(header) - 1 || millis() - _statusChange >= 1500) dropClient();
        return;
      }
      // Stream uses milliseconds; WiFiClient's overload uses seconds.
      _currentClient.Stream::setTimeout(250);
    }
    WebServer::handleClient();
  }
 private:
  void dropClient() {
    _currentClient.stop();
    _currentClient = WiFiClient();
    _currentStatus = HC_NONE;
  }
};
