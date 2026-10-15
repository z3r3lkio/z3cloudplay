// decode_qr_capture.swift - Independently verify a rendered QR capture on macOS.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

import CoreImage
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

guard CommandLine.arguments.count == 3 else {
    fail("usage: decode_qr_capture.swift CAPTURE.png EXPECTED_SYNTHETIC_URL")
}

let capture = URL(fileURLWithPath: CommandLine.arguments[1])
let expected = CommandLine.arguments[2]
guard expected.hasPrefix("https://synthetic.invalid/"), expected.utf8.count <= 4096,
      let image = CIImage(contentsOf: capture),
      let detector = CIDetector(ofType: CIDetectorTypeQRCode,
                                context: CIContext(options: nil),
                                options: [CIDetectorAccuracy: CIDetectorAccuracyHigh]) else {
    fail("QR capture verification could not initialize")
}

let payloads = detector.features(in: image).compactMap { feature in
    (feature as? CIQRCodeFeature)?.messageString
}
guard payloads.count == 1 else {
    fail("QR capture verification expected exactly one decodable symbol")
}
guard payloads[0] == expected else {
    // Do not print either value: a live challenge must never enter test output.
    fail("QR capture payload did not match the expected synthetic value")
}

print("QR capture verified")
