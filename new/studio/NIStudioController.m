//
//  NIStudioController.m
//  Macchina — NewClients/Shared
//
//  Wraps NIAgentConnection into the high-level Studio API. Everything here is the
//  distilled result of the reverse-engineering in docs/ and LEARNINGS.md; the
//  raw protocol constants are the only "magic" left.
//

#import "NIStudioController.h"
#import "NIAgentConnection.h"

const NSUInteger NIStudioLEDChannelCount = 213;
const uint16_t   NIStudioDisplayWidth    = 480;
const uint16_t   NIStudioDisplayHeight   = 272;
const int        NIStudioDisplayCount    = 2;
// Channels are the true (un-shifted) LED indices now that flushLEDs sends the
// count field. The empirical probe was done under the old +4 shift, so these are
// the probe values minus 4; the ring came from M2's capture (already un-shifted).
const NSUInteger NIStudioPadLEDBase      = 0;
const int        NIStudioPadCount        = 16;
const NSUInteger NIStudioGroupLEDBase    = 106;
const int        NIStudioGroupCount      = 8;
const NSUInteger NIStudioVULeftBase      = 158;
const NSUInteger NIStudioVURightBase     = 174;
const int        NIStudioVUSegments      = 16;
const NSUInteger NIStudioVUColorChannel  = 190;
const NSUInteger NIStudioJogRingBase     = 198;
const int        NIStudioJogRingSegments = 15;

// Clamp to the 7-bit direct-level range. 0 = off (a real level, not "no change").
static uint8_t NIClampLevel(uint8_t v) { return v > 127 ? 127 : v; }

// 3-byte message tags (dispatch masks id & 0xffffff); the 0x03 version prefix is
// applied by NIAgentConnection -messageIdForTag:.
enum
{
    kTagControllerAcquire = 0x434300,   // body 'strt' → claim display+input focus
    kTagDisplayDraw       = 0x647344,   // bulk display draw envelope
    kTagSetLEDs           = 0x6c7500,   // [msgid][213 raw level bytes]

    // inbound event ids, already masked to 3 bytes
    kEvtPads     = 0x504e00,            // {u32 padId, u32 state, f32 pressure}
    kEvtSwitches = 0x734e00,            // {u32 id, f32 state}
    kEvtKnobs    = 0x654e00,            // {u32 id, f32 delta}
    kEvtWheel    = 0x774e00,            // {u32 id, i32 delta}
};

static const uint32_t kStudioProductId = 0x1300;
static const uint8_t  kDisplayDataFormat = 0x20;   // uncompressed RGB565 (works once focused)

static void AppendU32LE(NSMutableData * d, uint32_t v) { [d appendBytes:&v length:4]; }
static void AppendBE16(NSMutableData * d, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    [d appendBytes:b length:2];
}

// The Studio display samples RGB565 as BIG-ENDIAN on the wire (verified live:
// native-endian 0xF800 "red" showed as blue, 0x001F "blue" showed as green — a
// clean byte-swap). Callers pass native little-endian uint16 pixels; swap here.
static NSData * ByteSwapped565(NSData * pixels)
{
    NSUInteger n = pixels.length / 2;
    NSMutableData * out = [NSMutableData dataWithLength:n * 2];
    const uint16_t * src = pixels.bytes;
    uint16_t       * dst = out.mutableBytes;
    for (NSUInteger i = 0; i < n; i++)
    {
        uint16_t v = src[i];
        dst[i] = (uint16_t)((v << 8) | (v >> 8));
    }
    return out;
}


@implementation NIStudioController
{
    NSString          * _serial;
    NIAgentConnection * _conn;
    NIAgentEndpoints  * _subscription;
    NIAgentEndpoints  * _controller;
    uint8_t             _ledState[213];
}

@synthesize connected = _connected;

+ (instancetype)studioWithSerial:(NSString *)serial
{
    NIStudioController * s = [self new];
    s->_serial = [serial copy];
    return s;
}

- (NSString *)serial { return _serial; }

// --- Connect ----------------------------------------------------------------

- (BOOL)connect
{
    _conn = [NIAgentConnection connectionToService:@"NIHWMainHandler"];
    if (_conn == nil)
    {
        NSLog(@"NIStudioController: NIHWMainHandler not found — is NIHardwareAgent running?");
        return NO;
    }
    if ([_conn negotiateVersion] == NO)
        return NO;

    // Subscription connection (device add/remove channel).
    NIDeviceConnectResponse * sub = [_conn subscribeToProduct:kStudioProductId
                                                    clientTag:kNIClientTagMaschine2
                                                         role:kNIClientRolePrimary];
    _subscription = [_conn openEndpointsFor:sub label:@"subscription"];
    if (_subscription)
    {
        _subscription.request.quiet = YES;
        _subscription.notifications.quiet = YES;
        [_conn registerNotificationPort:_subscription];
        [_conn sendSubscribeVoidOp:_subscription];
    }

    // Controller connection — this is the one that carries draws, LEDs, input.
    NIDeviceConnectResponse * conn = [_conn connectToProduct:kStudioProductId
                                                   clientTag:kNIClientTagMaschine2
                                                        role:kNIClientRolePrimary
                                                      serial:_serial];
    _controller = [_conn openEndpointsFor:conn label:@"controller"];
    if (_controller == nil)
    {
        NSLog(@"NIStudioController: controller connect failed (serial %@).", _serial);
        return NO;
    }
    _controller.request.quiet = YES;
    _controller.notifications.quiet = YES;
    [_conn registerNotificationPort:_controller];
    [_conn sendSubscribeVoidOp:_controller];

    __weak NIStudioController * weakSelf = self;
    _controller.onInboundFrame = ^(NSData * frame) { [weakSelf handleEventFrame:frame]; };

    // Let the receive port process the async MsgConnectionEstablished completion
    // before we claim focus (mirrors the proven ordering — LEARNINGS.md E17).
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    [self acquireFocus];

    // Prime the agent's LED report cache. updateLEDs only re-pushes a report that
    // differs from its last-sent copy; after prior clients / M2 that cache is stale,
    // so a lone single-channel write may not reach USB. Flashing all-on then all-off
    // forces every report to change twice, resyncing the cache to a known blank state.
    [self setAllLEDs:0x7f]; [self flushLEDs];
    [self setAllLEDs:0];    [self flushLEDs];

    _connected = YES;
    return YES;
}

- (void)acquireFocus
{
    uint32_t strt = 0x73747274;   // 'strt'
    [_conn sendTag:kTagControllerAcquire
              body:[NSData dataWithBytes:&strt length:4]
            toPort:_controller.request];
}

// --- Displays ---------------------------------------------------------------

- (void)drawDisplay:(int)index rgb565Pixels:(NSData *)pixels
{
    if (_controller == nil || index < 0 || index >= NIStudioDisplayCount)
        return;

    NSData * wire = ByteSwapped565(pixels);   // → big-endian for the wire
    uint16_t w = NIStudioDisplayWidth, h = NIStudioDisplayHeight;
    uint32_t payloadSize = 16 + (uint32_t)wire.length + 4;     // WriteWindow + px + EndOfUpdate

    NSMutableData * frame = [NSMutableData dataWithCapacity:0x14 + payloadSize];

    // 0x647344 envelope.
    AppendU32LE(frame, [_conn messageIdForTag:kTagDisplayDraw]);
    AppendU32LE(frame, (uint32_t)index);
    AppendU32LE(frame, 0);                                 // (originX<<16)|originY
    AppendU32LE(frame, ((uint32_t)w << 16) | h);           // (width<<16)|height
    AppendU32LE(frame, payloadSize);

    // WriteWindowRequest (opcode 0x0084) + big-endian geometry.
    uint8_t ww[8] = { 0x84, 0x00, (uint8_t)index, kDisplayDataFormat, 0, 0, 0, 0 };
    [frame appendBytes:ww length:sizeof(ww)];
    AppendBE16(frame, 0);         // left
    AppendBE16(frame, 0);         // top
    AppendBE16(frame, w);
    AppendBE16(frame, h);

    [frame appendData:wire];

    // EndOfUpdateRequest (opcode 0x0040).
    uint8_t eou[4] = { 0x40, 0x00, (uint8_t)index, 0x00 };
    [frame appendBytes:eou length:sizeof(eou)];

    [_controller.request postFrame:frame];   // fire-and-forget
}

- (void)fillDisplay:(int)index rgb565:(uint16_t)color
{
    NSUInteger count = (NSUInteger)NIStudioDisplayWidth * NIStudioDisplayHeight;
    NSMutableData * px = [NSMutableData dataWithLength:count * 2];
    uint16_t * p = px.mutableBytes;
    for (NSUInteger i = 0; i < count; i++)
        p[i] = color;
    [self drawDisplay:index rgb565Pixels:px];
}

// --- LEDs -------------------------------------------------------------------

- (void)setLED:(NSUInteger)channel level:(uint8_t)level
{
    if (channel < NIStudioLEDChannelCount)
        _ledState[channel] = level;
}

- (void)setAllLEDs:(uint8_t)level
{
    memset(_ledState, level, sizeof(_ledState));
}

- (void)setPadLED:(NSUInteger)pad red:(uint8_t)r green:(uint8_t)g blue:(uint8_t)b
{
    if (pad >= (NSUInteger)NIStudioPadCount)
        return;
    // Pads split into two RGB runs: 1–8 at ch 0.., 9–16 at ch 62.. (live-mapped).
    NSUInteger base = (pad < 8) ? (0 + pad * 3) : (62 + (pad - 8) * 3);
    [self setLED:base + 0 level:NIClampLevel(r)];
    [self setLED:base + 1 level:NIClampLevel(g)];
    [self setLED:base + 2 level:NIClampLevel(b)];
}

- (void)setGroupLED:(NSUInteger)group red:(uint8_t)r green:(uint8_t)g blue:(uint8_t)b
{
    if (group >= (NSUInteger)NIStudioGroupCount)
        return;
    NSUInteger base = NIStudioGroupLEDBase + group * 3;
    [self setLED:base + 0 level:NIClampLevel(r)];
    [self setLED:base + 1 level:NIClampLevel(g)];
    [self setLED:base + 2 level:NIClampLevel(b)];
}

- (void)setVU:(BOOL)left segments:(NSUInteger)segments
{
    NSUInteger base = left ? NIStudioVULeftBase : NIStudioVURightBase;
    for (int i = 0; i < NIStudioVUSegments; i++)
        [self setLED:base + i level:((NSUInteger)i < segments) ? 127 : 0];   // 0 = off
}

- (void)setVUBlue:(BOOL)blue
{
    [self setLED:NIStudioVUColorChannel level:(blue ? 0 : 127)];
}

- (void)setJogRingSegment:(NSUInteger)segment level:(uint8_t)level
{
    if (segment < (NSUInteger)NIStudioJogRingSegments)
        [self setLED:NIStudioJogRingBase + segment level:NIClampLevel(level)];
}

- (void)flushLEDs
{
    if (_controller == nil)
        return;
    NSMutableData * frame = [NSMutableData dataWithCapacity:8 + sizeof(_ledState)];
    AppendU32LE(frame, [_conn messageIdForTag:kTagSetLEDs]);
    AppendU32LE(frame, (uint32_t)sizeof(_ledState));  // count=213 (M2 sends this; without it the
                                                      // buffer lands shifted -4 and truncates the last 4 channels)
    [frame appendBytes:_ledState length:sizeof(_ledState)];

    static BOOL debug; static dispatch_once_t once;
    dispatch_once(&once, ^{ debug = (getenv("MACCHINA_LED_DEBUG") != NULL); });
    if (debug)
    {
        NSUInteger nonzero = 0, first = NSNotFound;
        for (NSUInteger i = 0; i < sizeof(_ledState); i++)
            if (_ledState[i]) { nonzero++; if (first == NSNotFound) first = i; }
        NSLog(@"flushLEDs: %lu-byte frame, %lu nonzero channels, first lit = %ld",
              (unsigned long)frame.length, (unsigned long)nonzero, (long)first);
    }

    [_controller.request postFrame:frame];   // fire-and-forget
}

// --- Inbound events ---------------------------------------------------------

- (void)handleEventFrame:(NSData *)frame
{
    // Common header: [u32 msgId][u32 seq][u32 tsNanos][u32 count] then records.
    if (frame.length < 16)
        return;
    const uint8_t * b = frame.bytes;
    uint32_t msgId, count;
    memcpy(&msgId, b + 0,  4);
    memcpy(&count, b + 12, 4);

    const uint8_t * rec = b + 16;
    NSUInteger avail = frame.length - 16;
    id<NIStudioControllerDelegate> d = self.delegate;

    switch (msgId & 0xffffff)
    {
        case kEvtPads:
            for (uint32_t i = 0; i < count && avail >= 12; i++, rec += 12, avail -= 12)
            {
                if (![d respondsToSelector:@selector(studioController:padEvent:)])
                    continue;
                NIStudioPadEvent e;
                memcpy(&e.padId,    rec + 0, 4);
                memcpy(&e.state,    rec + 4, 4);
                memcpy(&e.pressure, rec + 8, 4);
                [d studioController:self padEvent:e];
            }
            break;

        case kEvtSwitches:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                if (![d respondsToSelector:@selector(studioController:buttonId:down:)])
                    continue;
                uint32_t id_; float state;
                memcpy(&id_,   rec + 0, 4);
                memcpy(&state, rec + 4, 4);
                [d studioController:self buttonId:id_ down:(state >= 0.5f)];
            }
            break;

        case kEvtKnobs:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                if (![d respondsToSelector:@selector(studioController:knobId:delta:)])
                    continue;
                uint32_t id_; float delta;
                memcpy(&id_,   rec + 0, 4);
                memcpy(&delta, rec + 4, 4);
                [d studioController:self knobId:id_ delta:delta];
            }
            break;

        case kEvtWheel:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                if (![d respondsToSelector:@selector(studioController:wheelId:step:)])
                    continue;
                uint32_t id_; int32_t step;
                memcpy(&id_,  rec + 0, 4);
                memcpy(&step, rec + 4, 4);
                [d studioController:self wheelId:id_ step:step];
            }
            break;

        default:
            break;
    }
}

@end
