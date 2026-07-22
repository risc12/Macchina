//
//  NIRawPort.h
//  Macchina — NewClients/Shared
//
//  Raw-frame CFMessagePort wrappers for the v3-era clients.
//
//  NICommon's NIClient/NIServer are typed: they parse frames into NIMessage
//  subclasses and dispatch by class. The establish-handshake work is the
//  opposite problem — the frames we most care about are NOT decoded yet, and
//  the whole point is to see their exact bytes. These wrappers move raw
//  NSData frames (first 4 bytes = little-endian messageId) and hexdump every
//  frame in both directions via NITooling's NIDescribeFrame.
//

#import <Foundation/Foundation.h>

@interface NIRawPortClient : NSObject

@property (readonly) NSString * name;

/// When YES, suppress the per-frame hexdump logging (both directions). Off by
/// default so the research clients keep their trace; NIStudioController turns it
/// on for its high-frequency draw/LED ports.
@property (atomic) BOOL quiet;

/// nil if no port with that name exists (i.e. the agent is not running).
+ (instancetype)clientWithName:(NSString *)name;

/// frame's first 4 bytes must be the messageId; returns the reply (nil on
/// timeout / no reply). Logs both directions.
- (NSData *)sendFrame:(NSData *)frame;

/// Fire-and-forget: post the frame WITHOUT waiting for a reply. Returns as soon
/// as the message is sent — use for high-rate one-way traffic (LED/display
/// updates) where blocking on a reply would add jitter.
- (void)postFrame:(NSData *)frame;

@end


@class NIRawPortServer;

/// Return the reply to send back, or nil for no reply data.
typedef NSData * (^NIRawFrameHandler)(NIRawPortServer * server, NSData * frame);

@interface NIRawPortServer : NSObject

@property (readonly) NSString * name;

/// When YES, suppress the per-frame hexdump logging. Off by default.
@property (atomic) BOOL quiet;

/// nil if the name is already taken in the bootstrap namespace (stale port
/// from a dead client — replug the device or wait for the agent to clean up).
+ (instancetype)serverWithName:(NSString *)name handler:(NIRawFrameHandler)handler;

- (void)scheduleInRunLoop:(NSRunLoop *)runloop;

@end
