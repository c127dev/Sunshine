#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "src/config.h"
#include "src/logging.h"
#include "harec.h"

using namespace std::literals;

namespace platf {

  namespace {
    struct harec_img_t: public img_t {
      ~harec_img_t() override {
        if (data) delete[] data;
        data = nullptr;
      }
    };

    class harec_display_t: public display_t {
    public:
      harec_display_t(const std::string &display_name, int width, int height, int framerate):
          display_name(display_name),
          sockfd(-1),
          codec_ctx(nullptr),
          packet(nullptr),
          frame(nullptr),
          sws_ctx(nullptr) {
        this->width = width;
        this->height = height;
        this->framerate = framerate;

        // IP/Port configuration
        if (display_name.empty() || display_name == "harec") {
          host = "127.0.0.1";
          port = 8888;
        } else {
          auto pos = display_name.find(':');
          if (pos != std::string::npos) {
            host = display_name.substr(0, pos);
            port = std::stoi(display_name.substr(pos + 1));
          } else {
            host = display_name;
            port = 8888;
          }
        }
      }

      ~harec_display_t() {
        stop_capturing();
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (sws_ctx) sws_freeContext(sws_ctx);
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<harec_img_t>();
        img->width = width;
        img->height = height;

        // BGR0 (4 bytes per pixel) for Sunshine compatibility
        img->pixel_pitch = 4;
        img->row_pitch = width * 4;
        img->data = new uint8_t[width * height * 4];
        return img;
      }

      int dummy_img(img_t *img) override {
        std::memset(img->data, 0, width * height * 4);
        return 0;
      }

      void stop_capturing() {
        close_socket();
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        if (!init_decoder()) {
          return capture_e::error;
        }

        // Pre-allocate receive buffer to avoid reallocations (4MB initial)
        recv_buf.resize(4 * 1024 * 1024);

        while (true) {
          if (sockfd == -1) {
            if (!connect_server()) {
              std::this_thread::sleep_for(std::chrono::seconds(1));
              continue;
            }
          }

          // This prevents buffer bloat from causing increasing latency.
          bool frame_ready = false;
          uint32_t final_size = 0;

          while (true) {
            // Read the 4-byte size header
            uint32_t size_be;
            if (!read_n_bytes(sizeof(size_be), &size_be)) {
              close_socket();
              break;
            }
            uint32_t size = ntohl(size_be);

            // Safety check: reject absurdly large frames (>16MB)
            if (size > 16 * 1024 * 1024) {
              BOOST_LOG(error) << "HARec: Frame too large (" << size << " bytes), reconnecting";
              close_socket();
              break;
            }

            // Check how much data is queued in the kernel receive buffer
            int bytes_available = 0;
            if (ioctl(sockfd, FIONREAD, &bytes_available) < 0) bytes_available = 0;

            if (bytes_available > (int)(size + 4)) {
               skip_n_bytes_fast(size);
               continue;
            }

            // Read newest frame
            if (recv_buf.size() < size) recv_buf.resize(size);
            if (!read_n_bytes(size, recv_buf.data())) {
              close_socket();
              break;
            }

            final_size = size;
            frame_ready = true;
            break;
          }

          if (sockfd == -1) continue;
          if (!frame_ready) continue;

          // Pull image frame
          std::shared_ptr<img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          // Decode JPEG (CPU) and convert to BGR0
          if (decode_and_convert(recv_buf.data(), final_size, img)) {
            img->frame_timestamp = std::chrono::steady_clock::now();
            if (!push_captured_image_cb(std::move(img), true)) {
              return capture_e::interrupted;
            }
          }
        }
        return capture_e::ok;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
        return std::make_unique<avcodec_encode_device_t>();
      }

    private:
      std::string display_name;
      std::string host;
      int port;
      int sockfd;
      int framerate;

      AVCodecContext *codec_ctx;
      AVPacket *packet;
      AVFrame *frame;

      struct SwsContext *sws_ctx;
      int cached_w = 0, cached_h = 0, cached_fmt = -1;

      // Pre-allocated buffers to avoid per-frame allocations
      std::vector<uint8_t> recv_buf;
      static constexpr size_t SKIP_BUF_SIZE = 64 * 1024;  // 64KB skip buffer
      uint8_t skip_buf[SKIP_BUF_SIZE] = {};

      bool init_decoder() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
          BOOST_LOG(error) << "Codec MJPEG not found";
          return false;
        }

        codec_ctx = avcodec_alloc_context3(codec);

        // Low-latency decoder flags
        codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;  // Allow non-spec-compliant speedups
        codec_ctx->thread_count = 1;  // Single thread avoids frame-reordering latency

        // Skip loop filter and frame for speed (acceptable for MJPEG)
        codec_ctx->skip_loop_filter = AVDISCARD_ALL;
        codec_ctx->skip_frame = AVDISCARD_DEFAULT;

        if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
          BOOST_LOG(error) << "Could not open codec";
          return false;
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        return true;
      }

      bool connect_server() {
        struct sockaddr_in serv_addr;
        struct hostent *server;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return false;

        server = gethostbyname(host.c_str());
        if (server == NULL) {
          close(sockfd);
          sockfd = -1;
          return false;
        }

        std::memset((char *)&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        std::memcpy((char *)&serv_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
        serv_addr.sin_port = htons(port);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
          close(sockfd);
          sockfd = -1;
          return false;
        }

        // TCP_NODELAY
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

        // TCP_QUICKACK
        setsockopt(sockfd, IPPROTO_TCP, TCP_QUICKACK, (char *)&flag, sizeof(int));

        // Adds buffer to 2MB to absorb frame bursts without dropping
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        BOOST_LOG(info) << "Connected to " << host << ":" << port;
        return true;
      }

      void close_socket() {
        if (sockfd != -1) {
          close(sockfd);
          sockfd = -1;
        }
      }

      bool read_n_bytes(size_t n, void *buf) {
        size_t total_read = 0;
        uint8_t *ptr = (uint8_t *)buf;
        while (total_read < n) {
          ssize_t r = recv(sockfd, ptr + total_read, n - total_read, 0);
          if (r <= 0) return false;
          total_read += r;
        }
        return true;
      }

      // Efficiently skip n bytes without allocating memory per call
      void skip_n_bytes_fast(size_t n) {
        size_t total_read = 0;
        while (total_read < n) {
          size_t chunk = std::min(n - total_read, SKIP_BUF_SIZE);
          ssize_t r = recv(sockfd, skip_buf, chunk, 0);
          if (r <= 0) return;
          total_read += r;
        }
      }

      bool decode_and_convert(const uint8_t* data, size_t size, std::shared_ptr<img_t> &img) {
        packet->data = (uint8_t *)data;
        packet->size = size;

        if (avcodec_send_packet(codec_ctx, packet) < 0) return false;

        int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret < 0) return false;

        // Recreate swscale context
        if (!sws_ctx || cached_w != frame->width || cached_h != frame->height || cached_fmt != frame->format) {
          if (sws_ctx) sws_freeContext(sws_ctx);

          // SWS_POINT (nearest neighbor) is the fastest scaler
          sws_ctx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                   width, height, AV_PIX_FMT_BGR0,
                                   SWS_POINT, NULL, NULL, NULL);
          cached_w = frame->width;
          cached_h = frame->height;
          cached_fmt = frame->format;

          if (!sws_ctx) {
            BOOST_LOG(error) << "HARec: Failed to create swscale context";
            return false;
          }
        }

        uint8_t *dest[4] = { img->data, NULL, NULL, NULL };
        int dest_linesize[4] = { img->row_pitch, 0, 0, 0 };

        sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dest, dest_linesize);

        return true;
      }
    };
  }

  std::vector<std::string> harec_display_names() {
    return {"harec"};
  }

  std::shared_ptr<display_t> harec_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    return std::make_shared<harec_display_t>(display_name, config.width, config.height, config.framerate);
  }
}
