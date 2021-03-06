// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef HAS_NSIGHT_AFTERMATH

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_Defines.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

#include "common/common_paths.h"
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"

#include "video_core/renderer_vulkan/nsight_aftermath_tracker.h"

namespace Vulkan {

static constexpr char AFTERMATH_LIB_NAME[] = "GFSDK_Aftermath_Lib.x64.dll";

NsightAftermathTracker::NsightAftermathTracker() = default;

NsightAftermathTracker::~NsightAftermathTracker() {
    if (initialized) {
        (void)GFSDK_Aftermath_DisableGpuCrashDumps();
    }
}

bool NsightAftermathTracker::Initialize() {
    if (!dl.Open(AFTERMATH_LIB_NAME)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Nsight Aftermath DLL");
        return false;
    }

    if (!dl.GetSymbol("GFSDK_Aftermath_DisableGpuCrashDumps",
                      &GFSDK_Aftermath_DisableGpuCrashDumps) ||
        !dl.GetSymbol("GFSDK_Aftermath_EnableGpuCrashDumps",
                      &GFSDK_Aftermath_EnableGpuCrashDumps) ||
        !dl.GetSymbol("GFSDK_Aftermath_GetShaderDebugInfoIdentifier",
                      &GFSDK_Aftermath_GetShaderDebugInfoIdentifier) ||
        !dl.GetSymbol("GFSDK_Aftermath_GetShaderHashSpirv", &GFSDK_Aftermath_GetShaderHashSpirv) ||
        !dl.GetSymbol("GFSDK_Aftermath_GpuCrashDump_CreateDecoder",
                      &GFSDK_Aftermath_GpuCrashDump_CreateDecoder) ||
        !dl.GetSymbol("GFSDK_Aftermath_GpuCrashDump_DestroyDecoder",
                      &GFSDK_Aftermath_GpuCrashDump_DestroyDecoder) ||
        !dl.GetSymbol("GFSDK_Aftermath_GpuCrashDump_GenerateJSON",
                      &GFSDK_Aftermath_GpuCrashDump_GenerateJSON) ||
        !dl.GetSymbol("GFSDK_Aftermath_GpuCrashDump_GetJSON",
                      &GFSDK_Aftermath_GpuCrashDump_GetJSON)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Nsight Aftermath function pointers");
        return false;
    }

    dump_dir = Common::FS::GetUserPath(Common::FS::UserPath::LogDir) + "gpucrash";

    (void)Common::FS::DeleteDirRecursively(dump_dir);
    if (!Common::FS::CreateDir(dump_dir)) {
        LOG_ERROR(Render_Vulkan, "Failed to create Nsight Aftermath dump directory");
        return false;
    }

    if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_EnableGpuCrashDumps(
            GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
            GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default, GpuCrashDumpCallback,
            ShaderDebugInfoCallback, CrashDumpDescriptionCallback, this))) {
        LOG_ERROR(Render_Vulkan, "GFSDK_Aftermath_EnableGpuCrashDumps failed");
        return false;
    }

    LOG_INFO(Render_Vulkan, "Nsight Aftermath dump directory is \"{}\"", dump_dir);

    initialized = true;
    return true;
}

void NsightAftermathTracker::SaveShader(const std::vector<u32>& spirv) const {
    if (!initialized) {
        return;
    }

    std::vector<u32> spirv_copy = spirv;
    GFSDK_Aftermath_SpirvCode shader;
    shader.pData = spirv_copy.data();
    shader.size = static_cast<u32>(spirv_copy.size() * 4);

    std::scoped_lock lock{mutex};

    GFSDK_Aftermath_ShaderHash hash;
    if (!GFSDK_Aftermath_SUCCEED(
            GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &shader, &hash))) {
        LOG_ERROR(Render_Vulkan, "Failed to hash SPIR-V module");
        return;
    }

    Common::FS::IOFile file(fmt::format("{}/source_{:016x}.spv", dump_dir, hash.hash), "wb");
    if (!file.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Failed to dump SPIR-V module with hash={:016x}", hash.hash);
        return;
    }
    if (file.WriteArray(spirv.data(), spirv.size()) != spirv.size()) {
        LOG_ERROR(Render_Vulkan, "Failed to write SPIR-V module with hash={:016x}", hash.hash);
        return;
    }
}

void NsightAftermathTracker::OnGpuCrashDumpCallback(const void* gpu_crash_dump,
                                                    u32 gpu_crash_dump_size) {
    std::scoped_lock lock{mutex};

    LOG_CRITICAL(Render_Vulkan, "called");

    GFSDK_Aftermath_GpuCrashDump_Decoder decoder;
    if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
            GFSDK_Aftermath_Version_API, gpu_crash_dump, gpu_crash_dump_size, &decoder))) {
        LOG_ERROR(Render_Vulkan, "Failed to create decoder");
        return;
    }
    SCOPE_EXIT({ GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder); });

    u32 json_size = 0;
    if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
            decoder, GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
            GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE, nullptr, nullptr, nullptr, nullptr,
            this, &json_size))) {
        LOG_ERROR(Render_Vulkan, "Failed to generate JSON");
        return;
    }
    std::vector<char> json(json_size);
    if (!GFSDK_Aftermath_SUCCEED(
            GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, json_size, json.data()))) {
        LOG_ERROR(Render_Vulkan, "Failed to query JSON");
        return;
    }

    const std::string base_name = [this] {
        const int id = dump_id++;
        if (id == 0) {
            return fmt::format("{}/crash.nv-gpudmp", dump_dir);
        } else {
            return fmt::format("{}/crash_{}.nv-gpudmp", dump_dir, id);
        }
    }();

    std::string_view dump_view(static_cast<const char*>(gpu_crash_dump), gpu_crash_dump_size);
    if (Common::FS::WriteStringToFile(false, base_name, dump_view) != gpu_crash_dump_size) {
        LOG_ERROR(Render_Vulkan, "Failed to write dump file");
        return;
    }
    const std::string_view json_view(json.data(), json.size());
    if (Common::FS::WriteStringToFile(true, base_name + ".json", json_view) != json.size()) {
        LOG_ERROR(Render_Vulkan, "Failed to write JSON");
        return;
    }
}

void NsightAftermathTracker::OnShaderDebugInfoCallback(const void* shader_debug_info,
                                                       u32 shader_debug_info_size) {
    std::scoped_lock lock{mutex};

    GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier;
    if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GetShaderDebugInfoIdentifier(
            GFSDK_Aftermath_Version_API, shader_debug_info, shader_debug_info_size, &identifier))) {
        LOG_ERROR(Render_Vulkan, "GFSDK_Aftermath_GetShaderDebugInfoIdentifier failed");
        return;
    }

    const std::string path =
        fmt::format("{}/shader_{:016x}{:016x}.nvdbg", dump_dir, identifier.id[0], identifier.id[1]);
    Common::FS::IOFile file(path, "wb");
    if (!file.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Failed to create file {}", path);
        return;
    }
    if (file.WriteBytes(static_cast<const u8*>(shader_debug_info), shader_debug_info_size) !=
        shader_debug_info_size) {
        LOG_ERROR(Render_Vulkan, "Failed to write file {}", path);
        return;
    }
}

void NsightAftermathTracker::OnCrashDumpDescriptionCallback(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add_description) {
    add_description(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "yuzu");
}

void NsightAftermathTracker::GpuCrashDumpCallback(const void* gpu_crash_dump,
                                                  u32 gpu_crash_dump_size, void* user_data) {
    static_cast<NsightAftermathTracker*>(user_data)->OnGpuCrashDumpCallback(gpu_crash_dump,
                                                                            gpu_crash_dump_size);
}

void NsightAftermathTracker::ShaderDebugInfoCallback(const void* shader_debug_info,
                                                     u32 shader_debug_info_size, void* user_data) {
    static_cast<NsightAftermathTracker*>(user_data)->OnShaderDebugInfoCallback(
        shader_debug_info, shader_debug_info_size);
}

void NsightAftermathTracker::CrashDumpDescriptionCallback(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add_description, void* user_data) {
    static_cast<NsightAftermathTracker*>(user_data)->OnCrashDumpDescriptionCallback(
        add_description);
}

} // namespace Vulkan

#endif // HAS_NSIGHT_AFTERMATH
