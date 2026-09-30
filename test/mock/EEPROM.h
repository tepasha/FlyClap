#pragma once
#include <string.h>

struct MockEEPROM {
  unsigned char bytes[1024];
  MockEEPROM() { memset(bytes, 0xFF, sizeof(bytes)); }  // чиста EEPROM
  template <class T>
  T &get(int addr, T &v) {
    memcpy(&v, bytes + addr, sizeof(T));
    return v;
  }
  template <class T>
  const T &put(int addr, const T &v) {
    memcpy(bytes + addr, &v, sizeof(T));
    return v;
  }
};

extern MockEEPROM EEPROM;
