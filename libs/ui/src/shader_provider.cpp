#include "shader_provider.h"

#ifdef WS_UI_DEV_SHADER_RELOAD

// Dev-only shader hot reload. Not compiled into default builds: with the
// define off this TU is an empty translation unit around the header's
// trivial wrappers over the committed spirv.h words.

#include <ws/core/log.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace ws::ui::r2d::spirv {

namespace {

// Same flags as tools/compile_shaders.ps1 Compile-Spv for a glslang-family
// binary: -V for Vulkan semantics, vulkan1.0 target, optimized. The script
// is the single source of truth for the flags - these are comment-tied to
// it (both files say so). The bindless variant adds -DWS_BINDLESS and
// targets vulkan1.1: nonuniformEXT emits SPIR-V 1.3 decorations the
// vulkan1.0 environment does not accept.
constexpr char kCompileCommandLine[] =
    "\"{}\" -V --target-env vulkan1.0 -O \"{}\" -o \"{}\"";
constexpr char kCompileBindlessCommandLine[] =
    "\"{}\" -V --target-env vulkan1.1 -DWS_BINDLESS -O \"{}\" -o \"{}\"";

// The SPIR-V magic word: every valid module starts with it.
constexpr std::uint32_t kSpirvMagic = 0x07230203u;

// Last-good compiled words (empty until the first successful reload). A
// failed compile never touches these. The VkShaderModule wraps a COPY of
// the words (vkCreateShaderModule), so replacing them wholesale after a
// pipeline rebake is safe - no dangling code pointer exists.
std::vector<std::uint32_t> g_vert;
std::vector<std::uint32_t> g_frag;
std::vector<std::uint32_t> g_frag_bindless;

// File mtimes at the last compile attempt. Tracked even on failure: a
// broken shader must not re-trigger every frame (and the dev loop wants
// a retry only after the next save).
long long g_vert_mtime = 0;
long long g_frag_mtime = 0;
bool g_times_known = false;

bool file_exists(const std::string& path) {
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// getenv trips MSVC's C4996 deprecation under /W4 /WX; the Win32 call is
// the sanctioned spelling. Empty string = unset (getenv's null maps to
// "" here - both mean "not configured").
std::string env_value(const char* name) {
    char buffer[MAX_PATH];
    const DWORD size = GetEnvironmentVariableA(name, buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return "";
    }
    return buffer;
}

// Same resolution order as tools/compile_shaders.ps1 Find-Compiler, minus
// the -Compiler parameter (a build-time-only argument there): WS_GLSLANG,
// then PATH, then VULKAN_SDK\Bin, then the known %TEMP% drop.
std::string find_compiler() {
    const std::string env = env_value("WS_GLSLANG");
    if (!env.empty() && file_exists(env)) {
        return env;
    }
    const char* candidates[] = {"glslang.exe", "glslangValidator.exe",
                                "glslc.exe"};
    char buffer[MAX_PATH];
    for (const char* name : candidates) {
        if (SearchPathA(nullptr, name, nullptr, MAX_PATH, buffer, nullptr)) {
            return buffer;
        }
    }
    const std::string sdk = env_value("VULKAN_SDK");
    if (!sdk.empty()) {
        const std::string glslang = sdk + "\\Bin\\glslang.exe";
        if (file_exists(glslang)) {
            return glslang;
        }
        const std::string glslc = sdk + "\\Bin\\glslc.exe";
        if (file_exists(glslc)) {
            return glslc;
        }
    }
    const std::string temp = env_value("TEMP");
    if (!temp.empty()) {
        const std::string pinned =
            temp + "\\glslang-16.6.0\\bin\\glslang.exe";
        if (file_exists(pinned)) {
            return pinned;
        }
    }
    return "";
}

// FILETIME (100ns units since 1601) as one comparable integer.
long long file_mtime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER large{};
    large.LowPart = data.ftLastWriteTime.dwLowDateTime;
    large.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<long long>(large.QuadPart);
}

// Runs the compiler as a subprocess with stdout+stderr captured into
// `output`; true only on a zero exit code.
bool run_compiler_capture(const std::string& command_line,
                          std::string* output) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
        return false;
    }
    // The parent's read end must not be inherited: the child holds the
    // write end, so EOF reaches the parent exactly when the child exits.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::string cmdline = command_line;
    PROCESS_INFORMATION proc{};
    if (!CreateProcessA(nullptr, cmdline.data(), &inheritable, &inheritable,
                        TRUE, 0, nullptr, nullptr, &startup, &proc)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }
    CloseHandle(write_end);  // parent keeps only the read end

    std::string captured;
    char buffer[4096];
    DWORD read_bytes = 0;
    for (;;) {
        const BOOL ok = ReadFile(read_end, buffer, sizeof(buffer),
                                  &read_bytes, nullptr);
        if (!ok || read_bytes == 0) {
            break;
        }
        captured.append(buffer, read_bytes);
    }
    CloseHandle(read_end);

    WaitForSingleObject(proc.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(proc.hProcess, &exit_code);
    CloseHandle(proc.hProcess);
    CloseHandle(proc.hThread);
    if (output != nullptr) {
        *output = captured;
    }
    return exit_code == 0;
}

// Loads the compiled .spv as words. Every failure logs the stage, keeps
// the last good words in place, and returns false.
bool load_spv(const std::string& out_path, std::vector<std::uint32_t>* out) {
    std::ifstream file(out_path, std::ios::binary | std::ios::ate);
    if (!file) {
        ws::core::error("ui: shader hot reload - cannot open {}", out_path);
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || (static_cast<std::uint64_t>(size) % 4) != 0) {
        ws::core::error(
            "ui: shader hot reload - {} is empty or not word-aligned",
            out_path);
        return false;
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(words.data()),
              static_cast<std::streamsize>(size));
    if (!file) {
        ws::core::error("ui: shader hot reload - short read on {}", out_path);
        return false;
    }
    if (words[0] != kSpirvMagic) {
        ws::core::error(
            "ui: shader hot reload - {} is missing the SPIR-V magic word",
            out_path);
        return false;
    }
    *out = std::move(words);
    return true;
}

// Full stage compile: subprocess to a temp .spv, validate, load. On any
// failure logs the compiler's stderr (glslang prints errors there) and
// returns false - the caller keeps the last good words. `suffix` tells the
// bindless variant's command line (and keeps its .spv file apart).
bool compile_stage(const std::string& compiler, const std::string& source,
                   const char* command_line, const char* spv_suffix,
                   std::vector<std::uint32_t>* out) {
    const std::string temp = env_value("TEMP");
    if (temp.empty()) {
        return false;
    }
    static const std::string tmp_dir = [temp]() {
        std::string dir = temp + "\\ws-shaders-hot";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }();
    const std::string base =
        source.substr(source.find_last_of("\\/") + 1);
    const std::string out_path = tmp_dir + "\\" + base + spv_suffix + ".spv";

    const std::string cmdline =
        std::format(command_line, compiler, source, out_path);
    std::string compiler_output;
    if (!run_compiler_capture(cmdline, &compiler_output)) {
        ws::core::error("ui: shader hot reload - {} failed on {}:\n{}",
                         compiler, source, compiler_output);
        return false;
    }
    return load_spv(out_path, out);
}

}  // namespace

bool poll_reload(const std::string& shader_dir) {
    const std::string vert = shader_dir + "\\batch.vert";
    const std::string frag = shader_dir + "\\batch.frag";
    const long long vert_mtime = file_mtime(vert);
    const long long frag_mtime = file_mtime(frag);

    if (!g_times_known) {
        g_vert_mtime = vert_mtime;
        g_frag_mtime = frag_mtime;
        g_times_known = true;
        return false;
    }
    if (vert_mtime == g_vert_mtime && frag_mtime == g_frag_mtime) {
        return false;
    }

    // Consume the mtimes no matter what happens below - a broken shader
    // must not retrigger every frame; the next save retries.
    g_vert_mtime = vert_mtime;
    g_frag_mtime = frag_mtime;

    const std::string compiler = find_compiler();
    if (compiler.empty()) {
        ws::core::warn(
            "ui: shader hot reload - no compiler found (WS_GLSLANG, PATH, "
            "VULKAN_SDK\\Bin, %TEMP%\\glslang-16.6.0); keeping the "
            "committed words");
        return false;
    }

    std::vector<std::uint32_t> new_vert = g_vert;
    std::vector<std::uint32_t> new_frag = g_frag;
    std::vector<std::uint32_t> new_frag_bindless = g_frag_bindless;
    // Both frag variants: the tier is fixed at init, but a frag edit must
    // hot-reload whichever one the live pipelines use - compile both or
    // keep the last good words (all-or-nothing, like the pipeline rebake).
    if (!compile_stage(compiler, vert, kCompileCommandLine, "", &new_vert) ||
        !compile_stage(compiler, frag, kCompileCommandLine, "", &new_frag) ||
        !compile_stage(compiler, frag, kCompileBindlessCommandLine,
                       "-bindless", &new_frag_bindless)) {
        return false;  // compile_stage already logged the failure
    }
    g_vert = std::move(new_vert);
    g_frag = std::move(new_frag);
    g_frag_bindless = std::move(new_frag_bindless);
    ws::core::info("ui: shader hot reload - swapped in recompiled stages");
    return true;
}

ShaderWords vert_words() {
    if (!g_vert.empty()) {
        return {g_vert.data(), g_vert.size()};
    }
    return {kBatchVertSpv, sizeof(kBatchVertSpv) / sizeof(kBatchVertSpv[0])};
}

ShaderWords frag_words(bool bindless) {
    const std::vector<std::uint32_t>& hot =
        bindless ? g_frag_bindless : g_frag;
    if (!hot.empty()) {
        return {hot.data(), hot.size()};
    }
    if (bindless) {
        return {kBatchFragBindlessSpv,
                sizeof(kBatchFragBindlessSpv) / sizeof(kBatchFragBindlessSpv[0])};
    }
    return {kBatchFragSpv, sizeof(kBatchFragSpv) / sizeof(kBatchFragSpv[0])};
}

}  // namespace ws::ui::r2d::spirv

#endif  // WS_UI_DEV_SHADER_RELOAD
