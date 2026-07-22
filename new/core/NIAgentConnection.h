//
//  NIAgentConnection.h
//  Macchina — NewClients/Shared
//
//  The v3 connect/establish flow, shared by both new clients: NIHardwareAgent
//  and NIHostIntegrationAgent embed the same NI::NHL2 IPCServer/IPCConnection
//  code (docs/studio-investigation.md §5), so one implementation serves both.
//
//  Implements what the investigation decoded (§3, §6):
//    GetServiceVersion  0xNN536756        → 8-byte v3 reply {0x00020802, 3}
//    DeviceSubscribe    0xNN447500 "Du"   [product][clientTag][role][0]
//    DeviceConnect      0xNN444900        [product][clientTag][role][len][serial\0]
//    SetAsciiString     0xNN404300        register our notification port
//    Subscribe void-op  0xNN447143        real agent replies empty
//  where NN is the negotiated protocol version (0x02 → 0x03).
//
//  It also instruments the ONE still-undecoded step — the
//  MsgConnectionEstablished completion the agent sends back on OUR receive
//  port (§6, the root cause of every rejected draw). Every inbound frame
//  there is hexdumped, and the reply to the FIRST one is switchable at
//  runtime so decoding it is a re-run, not a recompile:
//
//    MACCHINA_ESTABLISH_REPLY = echo (default) | true | empty | none
//
//  ("none" reproduces the old broken behaviour — the nil reply that left the
//  ClientEntry unlinked.)
//

#import <Foundation/Foundation.h>
#import "NIRawPort.h"
#import "NIResponse.h"

// Well-known 4CCs (see docs/protocol.md).
#define kNIClientTagMaschine2  'NiM2'   // Maschine 2 software
#define kNIClientTagMaschine1  'NiMS'   // Maschine 1 software
#define kNIClientRolePrimary   'prmy'

// 3-byte message tags (dispatch is on messageId & 0xffffff).
enum
{
    kNITagGetServiceVersion  = 0x536756,
    kNITagDeviceSubscribe    = 0x447500,   // "Du"
    kNITagDeviceConnect      = 0x444900,
    kNITagSetAsciiString     = 0x404300,
    kNITagSubscribeVoidOp    = 0x447143,
    kNITagGetSerialNumber    = 0x436753,
    kNITagGetFirmwareVersion = 0x436746,
    kNITagDisplayDraw        = 0x647344,   // "Dsd"
};

/// One established connection: the agent-assigned request (in) port we send
/// to, and the receive (out) port we own, on which the agent's completion
/// handshake and later notifications arrive.
@interface NIAgentEndpoints : NSObject
@property (readonly) NSString        * label;
@property (readonly) NIRawPortClient * request;
@property (readonly) NIRawPortServer * notifications;

/// Called for every inbound frame on the receive port AFTER the initial
/// MsgConnectionEstablished handshake frame — i.e. input-event notifications.
/// Set by a higher layer (NIStudioController). The receive port always replies
/// nil to these; events are fire-and-forget.
@property (copy) void (^onInboundFrame)(NSData * frame);
@end


@interface NIAgentConnection : NSObject

@property (readonly) NIRawPortClient * mainPort;
@property (readonly) uint32_t          versionPrefix;   // 0x02 until v3 negotiated

/// nil if the service's main port doesn't exist (agent not running).
+ (instancetype)connectionToService:(NSString *)mainPortName;

/// (versionPrefix << 24) | tag — unless tag already uses all 4 bytes
/// (4-char-code messages like "SMap"), which are passed through unchanged.
- (uint32_t)messageIdForTag:(uint32_t)tag;

- (NSData *)sendTag:(uint32_t)tag body:(NSData *)body toPort:(NIRawPortClient *)port;
- (NSData *)sendTag:(uint32_t)tag body:(NSData *)body;   // on mainPort

/// GetServiceVersion. An 8-byte reply upgrades us to the v3 prefix.
- (BOOL)negotiateVersion;

/// "Du" — subscription connection (device add/remove notifications).
- (NIDeviceConnectResponse *)subscribeToProduct:(uint32_t)productId
                                      clientTag:(uint32_t)clientTag
                                           role:(uint32_t)role;

/// DeviceConnect — controller connection, trailing ASCII serial string.
- (NIDeviceConnectResponse *)connectToProduct:(uint32_t)productId
                                    clientTag:(uint32_t)clientTag
                                         role:(uint32_t)role
                                       serial:(NSString *)serial;

/// DeviceConnect variant with a UTF-16LE id string — the layout
/// MsgDeviceConnect::setIDString in NIHostIntegrationAgent suggests (§6.2).
- (NIDeviceConnectResponse *)connectToProduct:(uint32_t)productId
                                    clientTag:(uint32_t)clientTag
                                         role:(uint32_t)role
                                utf16IdString:(NSString *)idString;

/// Stand up both ports for a successful connect reply and schedule the
/// receive port (with the establish instrumentation) on the current run loop.
- (NIAgentEndpoints *)openEndpointsFor:(NIDeviceConnectResponse *)response
                                 label:(NSString *)label;

/// SetAsciiString(our notification port name) on the connection's request
/// port — the v3 "register" step. boh values are the v2 sentinels; the v3
/// ones differ but are undecoded (docs/protocol.md).
- (NSData *)registerNotificationPort:(NIAgentEndpoints *)endpoints;

/// Register an ARBITRARY notification-port name against a connection's request
/// port. Used to test LEARNINGS.md E9 hypothesis #3 — that the agent links a
/// controller ClientEntry to a subscription one by shared notification-port
/// identity. Registering the subscription's port name against the controller
/// request port makes both connections advertise the same identity.
- (NSData *)registerNotificationPortName:(NSString *)portName
                                  toPort:(NIRawPortClient *)requestPort;

/// 0x447143 — the v3 "subscribe" void-op; the real agent replies empty.
- (NSData *)sendSubscribeVoidOp:(NIAgentEndpoints *)endpoints;

@end
