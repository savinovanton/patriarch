#include "d3d11_common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pat::engine::d3d11 {

std::string hr_string(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

core::Error make_error(int code, const char* what, HRESULT hr) {
    return core::Error{code, std::string(what) + " (HRESULT " + hr_string(hr) + ")"};
}

std::string wide_to_utf8(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

core::Result<ComPtr<ID3DBlob>>
compile_hlsl(const std::string& source, const char* source_name, const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(
        source.data(), source.size(), source_name, nullptr, nullptr, entry, target, flags, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                               errors->GetBufferSize())
                                 : std::string("shader compilation failed");
        return core::Result<ComPtr<ID3DBlob>>::err(core::Error{21, std::move(msg)});
    }
    return core::Result<ComPtr<ID3DBlob>>::ok(std::move(code));
}

ShaderSource::ShaderSource(std::filesystem::path dir, std::string file_name, const char* embedded_text)
    : dir_(std::move(dir)), file_name_(std::move(file_name)), embedded_(embedded_text) {}

core::Result<std::string> ShaderSource::load() {
    if (dir_.empty()) {
        return core::Result<std::string>::ok(embedded_);
    }
    const auto path = dir_ / file_name_;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return core::Result<std::string>::err(core::Error{20, "cannot open shader file " + path.string()});
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::error_code ec;
    stamp_ = std::filesystem::last_write_time(path, ec);
    return core::Result<std::string>::ok(ss.str());
}

bool ShaderSource::changed() const {
    if (dir_.empty()) {
        return false;
    }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(dir_ / file_name_, ec);
    return !ec && stamp != stamp_;
}

}  // namespace pat::engine::d3d11
