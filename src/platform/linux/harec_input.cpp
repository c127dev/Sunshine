#include "harec_input.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Define buttons if not defined by input.h
#ifndef BUTTON_LEFT
  #define BUTTON_LEFT 1
#endif
#ifndef BUTTON_MIDDLE
  #define BUTTON_MIDDLE 2
#endif
#ifndef BUTTON_RIGHT
  #define BUTTON_RIGHT 3
#endif
#ifndef BUTTON_X1
  #define BUTTON_X1 4
#endif
#ifndef BUTTON_X2
  #define BUTTON_X2 5
#endif

using namespace std::literals;

// Protocol definitions
#define PKT_TYPE_KEYBOARD 1
#define PKT_TYPE_TOUCH    2
#define PKT_TYPE_MOUSE    3
#define PKT_TYPE_PEN      4

#pragma pack(push, 1)

// Packet structures

struct NetHeader {
  uint8_t type;
};

struct NetTouchData {
  uint8_t eventType; // 0=DOWN, 1=UP, 2=MOVE
  uint32_t pointerId;
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
};

struct NetMouseData {
  int16_t rel_x;
  int16_t rel_y;
  uint8_t buttons; // Bitmask: 1=Left, 2=Right, 4=Middle
  int8_t scroll_v;
  int8_t scroll_h;
};

struct NetPenData {
  uint8_t toolType; // 0=Pen, 1=Eraser
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
  uint8_t touching; // 1=Touch, 0=Hover
  uint8_t button;   // Side button
  uint8_t tilt;     // 0-90
  uint16_t rotation;// 0-360
};

struct NetKeyboardData {
  uint16_t keycode;
  uint8_t state; // 0=UP, 1=DOWN, 2=REPEAT
};

struct NetPacket {
  NetHeader header;
  union {
    NetTouchData touch;
    NetMouseData mouse;
    NetPenData pen;
    NetKeyboardData kbd;
  } data;
};

#pragma pack(pop)

namespace platf {
  static int sockfd = -1;
  static std::mutex sock_mutex;
  static uint8_t mouse_button_mask = 0;

  static void close_socket_nolock() {
    if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
    }
  }

  static bool ensure_connection() {
    if (sockfd >= 0) return true;

    std::string host = "127.0.0.1";
    int port = 8887;

    std::string display_name = config::video.output_name;
    if (!display_name.empty() && display_name != "harec") {
      auto pos = display_name.find(':');
      if (pos != std::string::npos) {
        try {
          host = display_name.substr(0, pos);
          port = std::stoi(display_name.substr(pos + 1)) - 1;
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "HARec Input: Failed to parse display port: " << e.what();
        }
      } else {
        host = display_name;
      }
    }

    BOOST_LOG(info) << "HARec Input: Connecting to " << host << ":" << port;

    int fd = socket(AF_INET, SOCK_STREAM, 0); // TCP
    if (fd < 0) {
      BOOST_LOG(error) << "HARec Input: Socket creation failed";
      return false;
    }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(int));

    int sndbuf = 16 * 1024;  // 16KB for input events
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct hostent *he = gethostbyname(host.c_str());
    if (!he) {
      BOOST_LOG(error) << "HARec Input: DNS resolution failed for " << host;
      close(fd);
      return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    // Timeout
    struct timeval timeout;
    timeout.tv_sec = 2; timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      BOOST_LOG(error) << "HARec Input: Connection failed to " << host << ":" << port;
      close(fd);
      return false;
    }

    sockfd = fd;
    BOOST_LOG(info) << "HARec Input: Connected!";
    return true;
  }

  static void send_packet(const NetPacket &pkt, size_t data_size) {
    std::lock_guard<std::mutex> lock(sock_mutex);

    if (!ensure_connection()) return;

    size_t total_size = sizeof(NetHeader) + data_size;
    ssize_t sent = send(sockfd, &pkt, total_size, MSG_NOSIGNAL);

    if (sent < 0) {
      BOOST_LOG(error) << "HARec Input: Send failed (Broken Pipe). Reconnecting next time.";
      close_socket_nolock();
    }
  }


  // Map  Moonlight VKEY to Linux KEY_*
  static const std::map<short, short> vkey_to_linux = {
    {0x08, KEY_BACKSPACE}, {0x09, KEY_TAB}, {0x0D, KEY_ENTER}, {0x10, KEY_LEFTSHIFT},
    {0x11, KEY_LEFTCTRL}, {0x14, KEY_CAPSLOCK}, {0x1B, KEY_ESC}, {0x20, KEY_SPACE},
    {0x21, KEY_PAGEUP}, {0x22, KEY_PAGEDOWN}, {0x23, KEY_END}, {0x24, KEY_HOME},
    {0x25, KEY_LEFT}, {0x26, KEY_UP}, {0x27, KEY_RIGHT}, {0x28, KEY_DOWN},
    {0x2C, KEY_SYSRQ}, {0x2D, KEY_INSERT}, {0x2E, KEY_DELETE},
    {0x30, KEY_0}, {0x31, KEY_1}, {0x32, KEY_2}, {0x33, KEY_3}, {0x34, KEY_4},
    {0x35, KEY_5}, {0x36, KEY_6}, {0x37, KEY_7}, {0x38, KEY_8}, {0x39, KEY_9},
    {0x41, KEY_A}, {0x42, KEY_B}, {0x43, KEY_C}, {0x44, KEY_D}, {0x45, KEY_E},
    {0x46, KEY_F}, {0x47, KEY_G}, {0x48, KEY_H}, {0x49, KEY_I}, {0x4A, KEY_J},
    {0x4B, KEY_K}, {0x4C, KEY_L}, {0x4D, KEY_M}, {0x4E, KEY_N}, {0x4F, KEY_O},
    {0x50, KEY_P}, {0x51, KEY_Q}, {0x52, KEY_R}, {0x53, KEY_S}, {0x54, KEY_T},
    {0x55, KEY_U}, {0x56, KEY_V}, {0x57, KEY_W}, {0x58, KEY_X}, {0x59, KEY_Y},
    {0x5A, KEY_Z},
    {0x5B, KEY_LEFTMETA}, {0x5C, KEY_RIGHTMETA},
    {0x60, KEY_KP0}, {0x61, KEY_KP1}, {0x62, KEY_KP2}, {0x63, KEY_KP3}, {0x64, KEY_KP4},
    {0x65, KEY_KP5}, {0x66, KEY_KP6}, {0x67, KEY_KP7}, {0x68, KEY_KP8}, {0x69, KEY_KP9},
    {0x6A, KEY_KPASTERISK}, {0x6B, KEY_KPPLUS}, {0x6D, KEY_KPMINUS}, {0x6E, KEY_KPDOT}, {0x6F, KEY_KPSLASH},
    {0x70, KEY_F1}, {0x71, KEY_F2}, {0x72, KEY_F3}, {0x73, KEY_F4}, {0x74, KEY_F5},
    {0x75, KEY_F6}, {0x76, KEY_F7}, {0x77, KEY_F8}, {0x78, KEY_F9}, {0x79, KEY_F10},
    {0x7A, KEY_F11}, {0x7B, KEY_F12},
    {0x90, KEY_NUMLOCK}, {0x91, KEY_SCROLLLOCK},
    {0xA0, KEY_LEFTSHIFT}, {0xA1, KEY_RIGHTSHIFT}, {0xA2, KEY_LEFTCTRL}, {0xA3, KEY_RIGHTCTRL},
    {0xA4, KEY_LEFTALT}, {0xA5, KEY_RIGHTALT},
    {0xBA, KEY_SEMICOLON}, {0xBB, KEY_EQUAL}, {0xBC, KEY_COMMA}, {0xBD, KEY_MINUS},
    {0xBE, KEY_DOT}, {0xBF, KEY_SLASH}, {0xC0, KEY_GRAVE},
    {0xDB, KEY_LEFTBRACE}, {0xDC, KEY_BACKSLASH}, {0xDD, KEY_RIGHTBRACE}, {0xDE, KEY_APOSTROPHE},
    {0xE2, KEY_102ND}
  };

  void harec_send_key(uint16_t modcode, bool release) {
    auto it = vkey_to_linux.find(modcode);
    if (it == vkey_to_linux.end()) {
        return;
    }

    NetPacket pkt;
    pkt.header.type = PKT_TYPE_KEYBOARD;
    pkt.data.kbd.keycode = it->second;
    pkt.data.kbd.state = release ? 0 : 1; // 0=UP, 1=DOWN

    send_packet(pkt, sizeof(NetKeyboardData));
  }

  void harec_send_mouse_move(int deltaX, int deltaY) {
    NetPacket pkt;
    pkt.header.type = PKT_TYPE_MOUSE;
    pkt.data.mouse.rel_x = (int16_t)deltaX;
    pkt.data.mouse.rel_y = (int16_t)deltaY;
    pkt.data.mouse.buttons = mouse_button_mask;
    pkt.data.mouse.scroll_v = 0;
    pkt.data.mouse.scroll_h = 0;

    send_packet(pkt, sizeof(NetMouseData));
  }

  void harec_send_mouse_abs(const touch_port_t &touch_port, float x, float y) {
    if (touch_port.width == 0 || touch_port.height == 0) return;

    NetPacket pkt;
    pkt.header.type = PKT_TYPE_TOUCH;
    pkt.data.touch.pointerId = 0; // Use slot 0 for mouse abs
    pkt.data.touch.eventType = 2; // MOVE

    // Normalize to 0-65535
    float norm_x = std::clamp(x / touch_port.width, 0.0f, 1.0f);
    float norm_y = std::clamp(y / touch_port.height, 0.0f, 1.0f);

    pkt.data.touch.x = (uint16_t)(norm_x * 65535.0f);
    pkt.data.touch.y = (uint16_t)(norm_y * 65535.0f);
    pkt.data.touch.pressure = 32000; // Simulated pressure for visibility

    send_packet(pkt, sizeof(NetTouchData));
  }

  void harec_send_mouse_button(int button, bool release) {
    uint8_t mask = 0;
    switch (button) {
      case BUTTON_LEFT:   mask = 1; break;
      case BUTTON_RIGHT:  mask = 2; break;
      case BUTTON_MIDDLE: mask = 4; break;
      default: return;
    }

    uint8_t current_mask;
    {
      std::lock_guard<std::mutex> lock(sock_mutex);
      if (!release) {
          mouse_button_mask |= mask;
      } else {
          mouse_button_mask &= ~mask;
      }
      current_mask = mouse_button_mask;
    }

    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PKT_TYPE_MOUSE;
    pkt.data.mouse.buttons = current_mask;

    send_packet(pkt, sizeof(NetMouseData));
  }

  void harec_send_scroll(int amount) {
    NetPacket pkt;
    pkt.header.type = PKT_TYPE_MOUSE;
    memset(&pkt.data.mouse, 0, sizeof(NetMouseData));
    pkt.data.mouse.buttons = mouse_button_mask;

    // Map high-res scroll or raw amount.
    pkt.data.mouse.scroll_v = (int8_t)std::clamp(amount, -127, 127);

    send_packet(pkt, sizeof(NetMouseData));
  }

  void harec_send_hscroll(int amount) {
    NetPacket pkt;
    pkt.header.type = PKT_TYPE_MOUSE;
    memset(&pkt.data.mouse, 0, sizeof(NetMouseData));
    pkt.data.mouse.buttons = mouse_button_mask;
    pkt.data.mouse.scroll_h = (int8_t)std::clamp(amount, -127, 127);

    send_packet(pkt, sizeof(NetMouseData));
  }

  void harec_send_touch(const touch_port_t &touch_port, const touch_input_t &touch) {
    NetPacket pkt;
    pkt.header.type = PKT_TYPE_TOUCH;
    pkt.data.touch.pointerId = touch.pointerId;
    pkt.data.touch.eventType = touch.eventType; // 0=DOWN, 1=UP, 2=MOVE

    // Normalize floats 0.0-1.0 to 0-65535
    pkt.data.touch.x = (uint16_t)(std::clamp(touch.x, 0.0f, 1.0f) * 65535.0f);
    pkt.data.touch.y = (uint16_t)(std::clamp(touch.y, 0.0f, 1.0f) * 65535.0f);

    // Map pressure (0.0-1.0 to 0-65535)
    pkt.data.touch.pressure = (uint16_t)(std::clamp(touch.pressureOrDistance, 0.0f, 1.0f) * 65535.0f);

    send_packet(pkt, sizeof(NetTouchData));
  }

  void harec_send_pen(const touch_port_t &touch_port, const pen_input_t &pen) {
    NetPacket pkt;
    pkt.header.type = PKT_TYPE_PEN;
    pkt.data.pen.toolType = pen.toolType; // 0=Pen, 1=Eraser

    pkt.data.pen.x = (uint16_t)(std::clamp(pen.x, 0.0f, 1.0f) * 65535.0f);
    pkt.data.pen.y = (uint16_t)(std::clamp(pen.y, 0.0f, 1.0f) * 65535.0f);

    bool touching = (pen.eventType == 0 || pen.eventType == 2) && (pen.pressureOrDistance > 0);

    pkt.data.pen.touching = touching ? 1 : 0;

    // Pressure map
    pkt.data.pen.pressure = (uint16_t)(std::clamp(pen.pressureOrDistance, 0.0f, 1.0f) * 65535.0f);

    // Handle Pen Side Button
    pkt.data.pen.button = (pen.penButtons & 1) ? 1 : 0;

    pkt.data.pen.tilt = pen.tilt;
    pkt.data.pen.rotation = pen.rotation;

    send_packet(pkt, sizeof(NetPenData));
  }

}  // namespace platf
