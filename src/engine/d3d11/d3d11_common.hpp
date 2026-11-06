#pragma once

// Internal helpers shared by the Direct3D 11 backend sources. Not part of the engine's public API.

#include "pat/core/result.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace pat::engine::d3d11 {

using Microsoft::WRL::ComPtr;

inline constexpr const char* kLogCat = "d3d11";

[[nodiscard]] std::string hr_string(HRESULT hr);
[[nodiscard]] core::Error make_error(int code, const char* what, HRESULT hr);
[[nodiscard]] std::string wide_to_utf8(const wchar_t* s);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view s);

/// Compiles one HLSL entry point. On failure the error message holds the compiler output.
[[nodiscard]] core::Result<ComPtr<ID3DBlob>>
compile_hlsl(const std::string& source, const char* source_name, const char* entry, const char* target);

/// An HLSL source that is either embedded in the executable or read from a directory so that edits
/// can be hot-reloaded during development.
class ShaderSource {
public:
    /// `dir` empty: always use `embedded_text`. Otherwise `dir/file_name` is the source.
    ShaderSource(std::filesystem::path dir, std::string file_name, const char* embedded_text);

    /// Returns the current source text and remembers the file's modification time.
    [[nodiscard]] core::Result<std::string> load();
    /// True if the file on disk has a different modification time than the last load().
    [[nodiscard]] bool changed() const;
    [[nodiscard]] const std::string& file_name() const noexcept { return file_name_; }

private:
    std::filesystem::path dir_;
    std::string file_name_;
    const char* embedded_;
    std::filesystem::file_time_type stamp_{};
};

}  // namespace pat::engine::d3d11
