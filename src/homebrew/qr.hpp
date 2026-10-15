// qr.hpp - Bounded renderer-neutral QR encoding.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace z3cloudplay::homebrew {

constexpr std::size_t kMaximumQrPayloadBytes = 4096;
constexpr int kQrQuietZoneModules = 4;

class QrMatrix {
public:
    QrMatrix() = default;
    QrMatrix(int size, std::vector<std::uint8_t> modules);

    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ <= 0 || modules_.empty(); }
    [[nodiscard]] bool at(int x, int y) const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& modules() const noexcept { return modules_; }

private:
    int size_ = 0;
    std::vector<std::uint8_t> modules_;
};

struct QrLayout {
    int module_pixels = 0;
    int side_pixels = 0;
    int offset_pixels = 0;
};

// Encodes valid UTF-8 at medium error correction. Empty text is a valid generic
// payload. Authentication UI must treat a missing challenge as a loading state.
QrMatrix encode_qr(const std::string& text, std::string* error = nullptr);

// Fits the matrix and quiet zone into a square without fractional modules.
QrLayout qr_layout(const QrMatrix& matrix, int available_pixels,
                   int quiet_zone = kQrQuietZoneModules) noexcept;

} // namespace z3cloudplay::homebrew
