// Host stand-in for cmaglie's FlashStorage: program flash, erased (0xFF) until written. config.cpp's fallback store.
#pragma once
#include <string.h>

template <class T>
struct FlashStorageClass {
  unsigned char data[sizeof(T)];
  FlashStorageClass() { erase(); }
  void erase() { memset(data, 0xFF, sizeof(data)); }  // what a firmware upload does
  void read(T *out) { memcpy((void *)out, data, sizeof(T)); }
  void write(const T &v) { memcpy(data, (const void *)&v, sizeof(T)); }
};

#define FlashStorage(name, T) FlashStorageClass<T> name
