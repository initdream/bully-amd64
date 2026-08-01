#include <iostream>
#include <string>

extern "C" void *get_fallback_decodeStringRef(void *thiz) {
  static std::string fallback;
  return &fallback;
}
