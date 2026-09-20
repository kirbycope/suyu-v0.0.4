// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;
using Shader::Backend::SPIRV::NUM_TEXTURE_COMPARE_WORDS;

[[nodiscard]] inline std::optional<PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        return std::nullopt;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return std::nullopt;
}

[[nodiscard]] inline u32 NumDescriptorEntries(const Shader::Info& info) {
    return Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           Shader::NumDescriptors(info.texture_buffer_descriptors) +
           Shader::NumDescriptors(info.image_buffer_descriptors) +
           Shader::NumDescriptors(info.texture_descriptors) +
           Shader::NumDescriptors(info.image_descriptors);
}

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
        return device->IsKhrPushDescriptorSupported() &&
               num_descriptors <= device->MaxPushDescriptors();
    }

    // TODO(crueter): utilize layout binding flags
    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor) const {
        if (bindings.empty()) {
            return nullptr;
        }
        const VkDescriptorSetLayoutCreateFlags flags =
            use_push_descriptor ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint =
                is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry) * descriptors[i].count;
        }
    }

    const Device* device{};
    bool is_compute{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
        ++texture_index;
    }

    /// Records the emulated depth compare bits of the texture PushTexture is about to push.
    void PushTextureCompare(u32 compare_bits) noexcept {
        if (texture_index < NUM_TEXTURE_COMPARE_WORDS * 8) {
            compare_words[texture_index / 8] |= (compare_bits & 15u) << ((texture_index % 8) * 4);
        }
    }

    const std::array<u32, NUM_TEXTURE_COMPARE_WORDS>& CompareData() const noexcept {
        return compare_words;
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    std::array<u32, NUM_TEXTURE_COMPARE_WORDS> compare_words{};
    u32 texture_index{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views,
                                 bool emulate_depth_compare) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    for (const auto& desc : info.texture_descriptors) {
        bool is_rescaled{};
        u32 compare_bits{};
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            VkImageView vk_image_view{image_view.Handle(desc.type)};
            if (vk_image_view == VK_NULL_HANDLE) {
                const VkImageView null_image_view{texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID).Handle(desc.type)};
                if (null_image_view != VK_NULL_HANDLE) vk_image_view = null_image_view;
            }
            const Sampler& sampler{texture_cache.GetSampler(sampler_id)};
            // Diagnostic: is every sampled 1600x900 image one that has been rendered to?
            if (image_view.size.width >= 1600 && image_view.size.height >= 900) {
                ++g_large_sampled_total;
                if (!IsKnownLargeRenderTarget(image_view.ImageHandle())) {
                    if (const u64 n = ++g_large_sampled_not_rt; n % 500 == 1) {
                        LOG_INFO(Render_Vulkan,
                                 "Sampled large image that was never a render target: {}x{} "
                                 "format {} ({} so far of {})",
                                 image_view.size.width, image_view.size.height,
                                 static_cast<u32>(image_view.format), n,
                                 g_large_sampled_total.load());
                    }
                }
            }
            const bool use_fallback_sampler{sampler.HasAddedAnisotropy() &&
                                            !image_view.SupportsAnisotropy()};
            VkSampler vk_sampler{use_fallback_sampler ? sampler.HandleWithDefaultAnisotropy()
                                                      : sampler.Handle()};
            if (sampler.HasLinearFiltering() &&
                VideoCore::Surface::IsPixelFormatInteger(image_view.format)) {
                vk_sampler = sampler.HandleWithNearestFilter();
            }
            // Diagnostic: which depth-format images are read as plain (non-compare) textures.
            if (!desc.is_depth && VideoCore::Surface::GetFormatType(image_view.format) !=
                                      VideoCore::Surface::SurfaceType::ColorTexture) {
                static std::atomic<u32> depth_as_color_reads{0};
                if (const u32 n = ++depth_as_color_reads; n % 2000 == 1) {
                    LOG_INFO(Render_Vulkan, "Depth-format image {} sampled as colour ({} so far)",
                             static_cast<u32>(image_view.format), n);
                }
            }
            if (desc.is_depth && sampler.HasDepthComparison()) {
                if (emulate_depth_compare && !image_view.SupportsDepthComparison()) {
                    // Colour-format texture compared against: the shader compares in code,
                    // so hand it the guest compare function and sample the plain float
                    // through a sampler without a compare op. Depth formats keep the native
                    // hardware compare.
                    compare_bits = sampler.DepthCompareFunc() |
                                   (sampler.HasLinearFiltering() ? 8u : 0u);
                    vk_sampler = sampler.HandleWithoutDepthComparison();
                } else if (!image_view.SupportsDepthComparison()) {
                    vk_sampler = sampler.HandleWithoutDepthComparison();
                }
            }
            guest_descriptor_queue.AddSampledImage(vk_image_view, vk_sampler);
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushTextureCompare(compare_bits);
        rescaling.PushTexture(is_rescaled);
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(vk_image_view);
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushImage(is_rescaled);
    }
}

} // namespace Vulkan
