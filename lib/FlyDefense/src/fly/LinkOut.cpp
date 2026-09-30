#include "LinkOut.h"
#include "Base.h"

namespace fly {

bool LinkOut::begin(Base &base) {
#if defined(ESP32)
  if (!base.claimPin(txPin_, name())) return false;
  port_.begin(baud_, SERIAL_8N1, -1, txPin_);  // лише TX
#else
  if (&port_ == &Serial) {
    base.configError(name(), F("Serial is the log port, use another UART"));
    return false;
  }
  port_.begin(baud_);
#endif
  return true;
}

void LinkOut::send(char kind, int a, int b, int c) {
  Command cmd;
  cmd.kind = kind;
  cmd.v[0] = (int16_t)a;
  cmd.v[1] = (int16_t)b;
  cmd.v[2] = (int16_t)c;
  cmd.send(port_);
}

}  // namespace fly
