#pragma once

#include "device_frame_profile.hpp"
#include "zenith_tab_runtime.hpp"

namespace zevryon::massivedoc {

ZenithTabRuntimeConfig make_zenith_tab_runtime_config(
    DeviceFrameProfile profile,
    LayoutConfig layout = {},
    SharedRecordLengthAuthority* record_length_authority = nullptr) noexcept;

} // namespace zevryon::massivedoc
