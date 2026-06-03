#pragma once

#include <cstdint>
#include <string>
#include "src/platform/common.h"

namespace platf {
  void harec_send_key(uint16_t modcode, bool release);
  void harec_send_mouse_move(int deltaX, int deltaY);
  void harec_send_mouse_abs(const touch_port_t &touch_port, float x, float y);
  void harec_send_mouse_button(int button, bool release);
  void harec_send_scroll(int amount);
  void harec_send_hscroll(int amount);
  void harec_send_touch(const touch_port_t &touch_port, const touch_input_t &touch);
  void harec_send_pen(const touch_port_t &touch_port, const pen_input_t &pen);
}
