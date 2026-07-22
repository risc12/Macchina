//
//  NIReplay.h
//  Macchina — NITooling
//
//  The other half of NICaptureTap: read the length-prefixed capture files back
//  into frames so you can (a) inspect them offline, or (b) inject them into a
//  live port — e.g. replay a captured NIDisplayDrawMessage into the *real*
//  NIHardwareAgent and watch the physical panel to confirm a decoded format.
//

#import <Foundation/Foundation.h>

/// Parse a capture file (uint32 length + bytes, repeated) into its frames.
/// Returns nil if the file can't be read; a truncated trailing record is
/// dropped rather than treated as an error.
NSArray<NSData *> * NIReadCaptureFile(NSString * path);

/// Print NIDescribeFrame for every frame in a capture file. Pure inspection,
/// no hardware needed.
void NIDescribeCaptureFile(NSString * path);

/// Send each raw frame to a remote CFMessagePort by name, wrapping bytes in an
/// NIUnknownMessage so NIClient ships them verbatim. Requires the target port
/// (the real agent, or MacchinaServer) to be up. Returns the number of frames
/// sent that got a non-nil reply.
NSUInteger NISendFramesToPort(NSArray<NSData *> * frames, NSString * portName);
