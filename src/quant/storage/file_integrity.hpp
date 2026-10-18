#pragma once

#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace quant::storage::files {

// Standard SHA-256; no signing/authenticity claim. Tested against published known-answer vectors.
inline std::string sha256(const std::span<const std::byte> input) {
    constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<std::byte> bytes(input.begin(), input.end());
    const auto bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(std::byte{128});
    while (bytes.size() % 64 != 56) { bytes.push_back(std::byte{0}); }
    for (unsigned index = 0; index < 8; ++index) { bytes.push_back(static_cast<std::byte>((bits >> ((7 - index) * 8)) & 255)); }
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (unsigned index = 0; index < 16; ++index) {
            for (unsigned byte = 0; byte < 4; ++byte) { w[index] = (w[index] << 8) | std::to_integer<unsigned>(bytes[offset + index * 4 + byte]); }
        }
        for (unsigned index = 16; index < 64; ++index) {
            const auto s0 = std::rotr(w[index - 15], 7) ^ std::rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const auto s1 = std::rotr(w[index - 2], 17) ^ std::rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        auto [a,b,c,d,e,f,g,v] = h;
        for (unsigned index = 0; index < 64; ++index) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temp1 = v + s1 + choose + k[index] + w[index];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto temp2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
            v=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=v;
    }
    std::ostringstream output; output << std::hex << std::setfill('0');
    for (const auto value : h) { output << std::setw(8) << value; }
    return output.str();
}
inline std::string sha256(const std::string& input) {
    return sha256(std::as_bytes(std::span{input.data(), input.size()}));
}
inline void no_symlink(const std::filesystem::path& path) {
    for (auto component = path; !component.empty(); component = component.parent_path()) {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(component))) {
            throw std::invalid_argument("symlinked artifact component is not supported");
        }
        if (component == component.parent_path()) { break; }
    }
}
inline void sync(const std::filesystem::path& path) {
    no_symlink(path);
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) { throw std::system_error(errno, std::generic_category(), "open for fsync"); }
    const int result = ::fsync(descriptor); const int saved = errno;
    ::close(descriptor);
    if (result != 0) { throw std::system_error(saved, std::generic_category(), "fsync"); }
}
inline void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> bytes, const bool append = false) {
    no_symlink(path);
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | (append ? O_APPEND : O_EXCL), 0600);
    if (descriptor < 0) { throw std::system_error(errno, std::generic_category(), "exclusive artifact write"); }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) { continue; }
        if (count <= 0) { const auto saved = errno; ::close(descriptor); throw std::system_error(saved, std::generic_category(), "artifact write"); }
        written += static_cast<std::size_t>(count);
    }
    const int result = ::fsync(descriptor); const int saved = errno; ::close(descriptor);
    if (result != 0) { throw std::system_error(saved, std::generic_category(), "artifact fsync"); }
}
inline void write_text(const std::filesystem::path& path, const std::string& text) {
    write_bytes(path, std::as_bytes(std::span{text.data(), text.size()}));
}
inline std::string read(const std::filesystem::path& path) {
    no_symlink(path);
    const auto size = std::filesystem::file_size(path);
    if (size > 512ULL * 1024 * 1024) { throw std::invalid_argument("artifact exceeds first-slice read bound"); }
    std::ifstream input{path, std::ios::binary};
    if (!input) { throw std::runtime_error("required artifact is unavailable"); }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) { throw std::runtime_error("artifact changed/truncated while reading"); }
    return bytes;
}
inline void publish_marker(const std::filesystem::path& temporary, const std::filesystem::path& final) {
    no_symlink(temporary); no_symlink(final);
    if (::link(temporary.c_str(), final.c_str()) != 0) { throw std::system_error(errno, std::generic_category(), "non-replacing publication"); }
    sync(final.parent_path());
}
inline void create_directories(const std::filesystem::path& path) {
    no_symlink(path); std::filesystem::create_directories(path); no_symlink(path);
}
inline void register_text(const std::filesystem::path& path, const std::string& text) {
    if (std::filesystem::exists(path)) {
        if (read(path) != text) { throw std::invalid_argument("immutable catalog/acquisition reassignment"); }
        return;
    }
    write_text(path, text); sync(path.parent_path());
}
} // namespace quant::storage::files
