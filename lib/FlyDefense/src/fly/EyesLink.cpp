#include "EyesLink.h"
#include "Base.h"

namespace fly {

bool EyesLink::begin(Base &base) {
  if (!base.claim(Res::SerialRx, name())) return false;
  base.log().println(F("Waiting for ESP32 eyes..."));
  return true;
}

void EyesLink::tick(Base &, uint32_t now) {
  while (in_.available()) {
    const char *line = reader_.push((char)in_.read());
    Command c;
    if (!line || !c.parse(line)) continue;  // сміття — ігноруємо, зв'язок не оновлюємо
    handle(c);
    lastLinkAt_ = now;
  }
  const bool ok = now - lastLinkAt_ < LINK_TIMEOUT_MS;
  if (!ok && linkOk_) target_.cancel();
  linkOk_ = ok;
}

void EyesLink::handle(const Command &c) {
  switch (c.kind) {
    case 'A': target_.aim(c.v[0], c.v[1], c.v[2] != 0); break;
    case 'M': target_.moveTo(c.v[0], c.v[1]); break;
    case 'F':
      if (c.v[0] > 0) target_.testShot((uint16_t)c.v[0]);
      break;
    case 'B': target_.bigObject(); break;
  }
}

}  // namespace fly
