//
//  NIHexDump.h
//  Macchina — NITooling
//
//  Reverse-engineering tooling. Kept separate from NICommon so the protocol
//  library stays clean; nothing here is required to run the client/server.
//
//  Raw-frame inspection helpers: turn opaque CFData blobs into something a
//  human can diff against docs/protocol.md.
//

#import <Foundation/Foundation.h>

/// Classic offset / hex / ASCII dump of an arbitrary blob.
NSString * NIHexDump(NSData * data);

/// One-line identification of a wire frame (messageId + mapped class name +
/// length) followed by a full hexdump. Does NOT parse the body, so it is safe
/// on malformed or not-yet-understood frames — exactly what you want while
/// capturing unknown traffic.
NSString * NIDescribeFrame(NSData * data);
