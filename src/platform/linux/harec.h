/**
 * @file src/platform/linux/harec.h
 * @brief Declarations for HARec capture.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// local includes
#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  std::vector<std::string> harec_display_names();
  std::shared_ptr<display_t> harec_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
}
