#include "Protocol.h"
#include <stdlib.h>

namespace fly {

namespace {
// До n цілих після літери команди. Повертає, скільки прочитано.
uint8_t parseInts(const char *s, int16_t *v, uint8_t n) {
  uint8_t k = 0;
  while (k < n) {
    while (*s == ' ') s++;
    if (!*s) break;
    char *end;
    long x = strtol(s, &end, 10);
    if (end == s) break;
    v[k++] = (int16_t)x;
    s = end;
  }
  return k;
}
}  // namespace

bool Command::parse(const char *line) {
  kind = line[0];
  switch (kind) {
    case 'A': return parseInts(line + 1, v, 3) == 3;
    case 'M': return parseInts(line + 1, v, 2) == 2;
    case 'F': return parseInts(line + 1, v, 1) == 1;
    case 'B': return true;
    default: return false;
  }
}

void Command::send(Print &out) const {
  out.print(kind);
  const uint8_t n = kind == 'A' ? 3 : kind == 'M' ? 2 : kind == 'F' ? 1 : 0;
  for (uint8_t i = 0; i < n; i++) {
    out.print(' ');
    out.print(v[i]);
  }
  out.print('\n');
}

const char *LineReader::push(char c) {
  if (c == '\r') return nullptr;
  if (c == '\n') {
    buf_[len_] = 0;
    const bool ok = !overflow_ && len_;
    len_ = 0;
    overflow_ = false;
    return ok ? buf_ : nullptr;
  }
  if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;
  else overflow_ = true;
  return nullptr;
}

}  // namespace fly
