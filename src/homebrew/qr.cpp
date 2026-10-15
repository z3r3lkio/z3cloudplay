// qr.cpp - Bounded renderer-neutral QR encoding.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/qr.hpp"

#include "qrcodegen.h"

#include <limits>
#include <utility>

namespace z3cloudplay::homebrew {
namespace {

bool valid_utf8(const std::string& text) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index++];
        if (first <= 0x7F)
            continue;

        std::uint32_t codepoint = 0;
        int continuation_count = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            codepoint = first & 0x1FU;
            continuation_count = 1;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            codepoint = first & 0x0FU;
            continuation_count = 2;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            codepoint = first & 0x07U;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (text.size() - index < static_cast<std::size_t>(continuation_count))
            return false;
        for (int continuation = 0; continuation < continuation_count; ++continuation) {
            const unsigned char next = bytes[index++];
            if ((next & 0xC0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
            return false;
    }
    return true;
}

void set_error(std::string* error, const char* message)
{
    if (error != nullptr)
        *error = message;
}

} // namespace

QrMatrix::QrMatrix(int size, std::vector<std::uint8_t> modules)
    : size_(size), modules_(std::move(modules))
{
    if (size_ <= 0 || size_ > 177 ||
        modules_.size() != static_cast<std::size_t>(size_ * size_)) {
        size_ = 0;
        modules_.clear();
    }
}

bool QrMatrix::at(int x, int y) const noexcept
{
    if (x < 0 || y < 0 || x >= size_ || y >= size_)
        return false;
    return modules_[static_cast<std::size_t>(y * size_ + x)] != 0;
}

QrMatrix encode_qr(const std::string& text, std::string* error)
{
    if (error != nullptr)
        error->clear();
    if (text.size() > kMaximumQrPayloadBytes) {
        set_error(error, "QR payload exceeds the 4096-byte input limit");
        return {};
    }
    if (text.find('\0') != std::string::npos) {
        set_error(error, "QR payload contains an embedded NUL byte");
        return {};
    }
    if (!valid_utf8(text)) {
        set_error(error, "QR payload is not valid UTF-8");
        return {};
    }

    std::vector<std::uint8_t> temporary(qrcodegen_BUFFER_LEN_MAX);
    std::vector<std::uint8_t> encoded(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(text.c_str(), temporary.data(), encoded.data(),
                              qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        set_error(error, "QR payload exceeds version 40 capacity");
        return {};
    }

    const int size = qrcodegen_getSize(encoded.data());
    std::vector<std::uint8_t> modules(static_cast<std::size_t>(size * size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            modules[static_cast<std::size_t>(y * size + x)] =
                qrcodegen_getModule(encoded.data(), x, y) ? 1 : 0;
        }
    }
    return QrMatrix(size, std::move(modules));
}

QrLayout qr_layout(const QrMatrix& matrix, int available_pixels, int quiet_zone) noexcept
{
    if (matrix.empty() || available_pixels <= 0 || quiet_zone < 0 ||
        quiet_zone > (std::numeric_limits<int>::max() - matrix.size()) / 2)
        return {};
    const int total_modules = matrix.size() + quiet_zone * 2;
    const int module_pixels = available_pixels / total_modules;
    if (module_pixels <= 0)
        return {};
    const int side_pixels = module_pixels * total_modules;
    return {module_pixels, side_pixels, (available_pixels - side_pixels) / 2};
}

} // namespace z3cloudplay::homebrew
