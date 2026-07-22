//
//  NIImageDecode.h
//  Macchina — NITooling
//
//  The inverse of NI24BPPToST7529Data(): unpack a captured display payload
//  back into something you can look at. This is the single most useful tool
//  for cracking the display format — dump a captured NIDisplayDrawMessage
//  payload to a PGM and eyeball whether it's 1-bit, grayscale, or compressed.
//
//  Pure functions, no hardware, no Foundation run loop — the natural home for
//  unit tests (see NIToolingTests).
//

#import <Foundation/Foundation.h>

/// Decode an ST7529-packed payload (3 pixels → 2 bytes, 5 bits/pixel) into an
/// 8-bit grayscale buffer of width*height bytes. Each 5-bit level is expanded
/// across the full 0–255 range, so intermediate grays (if NI ever sends them)
/// survive the round-trip — the current encoder only emits 0x00 / 0x1f, i.e.
/// pure black / white.
///
/// Returns nil if `encoded` is too short for width*height pixels.
NSData * NIST7529DataToGray8(uint16_t width, uint16_t height, NSData * encoded);

/// Convenience: decode and write a binary PGM (P5) to `path`. PGM keeps this
/// Foundation-only (no ImageIO) and opens in Preview/GIMP/most viewers.
/// Returns NO on decode failure or write error.
BOOL NIST7529WritePGM(uint16_t width, uint16_t height, NSData * encoded, NSString * path);
