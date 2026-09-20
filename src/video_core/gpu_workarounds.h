// SPDX-FileCopyrightText: 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace VideoCore {

/// When set, a colour render target whose address equals an earlier bound target's address is
/// dropped from the render pass. Guests bind one surface at several slots; Vulkan drivers on
/// desktop tolerate it, Metal declares it undefined, and on Apple's tile-based GPUs the aliased
/// attachments overwrite each other's tiles when the pass is stored.
inline std::atomic<bool> dedupe_aliased_render_targets{false};

} // namespace VideoCore
