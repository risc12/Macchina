//
//  NICaptureTap.h
//  Macchina — NITooling
//
//  A zero-touch capture facility. Rather than edit NIServer, it swizzles
//  -handleIncomingData: so every raw inbound frame is (a) logged via
//  NIDescribeFrame and (b) appended to a per-port capture file, then passed
//  through untouched. NICommon behaviour is unchanged when the tap is absent.
//
//  Capture file format (little-endian), one record per frame:
//      uint32_t length; uint8_t bytes[length];
//  NIReplay reads this back.
//
//  Enable either explicitly with +installToDirectory:, or automatically by
//  setting MACCHINA_CAPTURE_DIR in the environment before launch (handled in
//  +load, so no code change to main.m is needed — just link NITooling).
//

#import <Foundation/Foundation.h>

@interface NICaptureTap : NSObject

/// Swizzle the tap into NIServer and write capture files under `dir`.
/// Idempotent. Returns NO if `dir` can't be created.
+ (BOOL)installToDirectory:(NSString *)dir;

@end
