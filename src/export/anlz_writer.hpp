#pragma once
#include <cstdint>
#include <string>

#include "bytes.hpp"
#include "deckhand/export/export.hpp"

namespace deckhand::usbexport::detail {

struct AnlzFiles {
  Bytes dat, ext, ex2;  // ex2 empty when not requested
};

AnlzFiles build_anlz(const ExportTrack& t, const std::string& audio_db_path, std::uint64_t file_size,
                     const ExportOptions& opt);

}  // namespace deckhand::usbexport::detail
