//
//  NIStudioController.h
//  Macchina — NewClients/Shared
//
//  A clean, high-level interface to a Maschine Studio, hiding the whole
//  reverse-engineered v3 recipe (docs/protocol.md, LEARNINGS.md E17–E18):
//
//    connect → subscribe (Du) → DeviceConnect(serial) → acquire display focus
//
//  Once connected you can push pixels to the two 480×272 colour displays, set
//  any of the 213 LED channels, and receive pad / button / knob / wheel input
//  through the delegate. All the CFMessagePort transport and the undecoded
//  handshake live in NIAgentConnection underneath.
//
//  Requires NIHardwareAgent RUNNING and a Studio plugged in. Schedules its
//  event port on the current run loop, so drive a run loop after -connect.
//

#import <Foundation/Foundation.h>

/// One pad event. state: 1 = hit (note-on), 4 = pressure update (aftertouch),
/// 3 = release (note-off). pressure is 0.0–1.0.
typedef struct
{
    uint32_t padId;      // 0–15
    uint32_t state;
    float    pressure;
} NIStudioPadEvent;

typedef NS_ENUM(uint32_t, NIStudioPadState)
{
    NIStudioPadHit      = 1,
    NIStudioPadRelease  = 3,
    NIStudioPadPressure = 4,
};

extern const NSUInteger NIStudioLEDChannelCount;  // 213
extern const uint16_t   NIStudioDisplayWidth;     // 480
extern const uint16_t   NIStudioDisplayHeight;    // 272
extern const int        NIStudioDisplayCount;     // 2

// The 16 RGB pads are two runs of 3-channel R,G,B triplets (live-mapped 2026-07):
// pads 1–8 at channels 4..27, pads 9–16 at channels 66..89. -setPadLED: handles
// the split; NIStudioPadLEDBase is the base of the first run.
extern const NSUInteger NIStudioPadLEDBase;       // 4
extern const int        NIStudioPadCount;         // 16

// The 8 RGB group buttons (A–H) are one run of 3-channel R,G,B triplets at
// channels 110..133 (group g → 110+3g). Live-mapped 2026-07.
extern const NSUInteger NIStudioGroupLEDBase;     // 110
extern const int        NIStudioGroupCount;       // 8

// The two VU meters are mono segment ladders (live-mapped 2026-07): left at
// channels 162..177, right at 178..193, 16 segments each (bottom→top, top = red).
extern const NSUInteger NIStudioVULeftBase;       // 158
extern const NSUInteger NIStudioVURightBase;      // 174
extern const int        NIStudioVUSegments;       // 16
// The VU segments are bi-colour; this one channel flips BOTH meters' colour
// (M2: off = input-monitor colour (blue), lit = normal output colour).
extern const NSUInteger NIStudioVUColorChannel;   // 190 (LED_LEVEL_COLOR)

// The jog-wheel ring: 15 mono segments at channels 198..212 — a FULLY CLOSED ring
// when all are lit (verified against M2's Mix mode). Segments 198..201 sit behind
// the top function labels (Browser/Tune/Swing/Volume), so lit individually they read
// as "labels", but they are the ring's top-left arc and close the circle.
extern const NSUInteger NIStudioJogRingBase;      // 198
extern const int        NIStudioJogRingSegments;  // 15

@class NIStudioController;

@protocol NIStudioControllerDelegate <NSObject>
@optional
/// A pad was hit, its pressure changed, or it was released.
- (void)studioController:(NIStudioController *)controller padEvent:(NIStudioPadEvent)event;
/// A button (or a touch-sensitive knob's touch sensor) went down/up.
- (void)studioController:(NIStudioController *)controller buttonId:(uint32_t)buttonId down:(BOOL)down;
/// A touch-sensitive knob turned (fine continuous delta, no detents).
- (void)studioController:(NIStudioController *)controller knobId:(uint32_t)knobId delta:(float)delta;
/// The detented big wheel turned by whole steps (+1 / -1 per notch).
- (void)studioController:(NIStudioController *)controller wheelId:(uint32_t)wheelId step:(int32_t)step;
@end


@interface NIStudioController : NSObject

@property (weak)     id<NIStudioControllerDelegate> delegate;
@property (readonly, getter=isConnected) BOOL connected;
@property (readonly) NSString * serial;

/// Maschine Studio (USB product 0x1300) with the given device serial.
+ (instancetype)studioWithSerial:(NSString *)serial;

/// Full connect + focus. NO if the agent isn't running or the device refused.
- (BOOL)connect;

/// (Re)claim display + input focus. -connect already does this; call again if
/// another client may have stolen focus.
- (void)acquireFocus;

// --- Displays (index 0 = left, 1 = right) -----------------------------------

/// Push a full 480×272 RGB565 frame (little-endian uint16 per pixel,
/// width*height*2 = 261120 bytes) to a display.
- (void)drawDisplay:(int)index rgb565Pixels:(NSData *)pixels;

/// Fill a whole display with one RGB565 colour.
- (void)fillDisplay:(int)index rgb565:(uint16_t)color;

// --- LEDs (213 channels) -----------------------------------------------------
// Intensity is 7-bit direct level: 0 = off, 127 = full. Values ≥128 switch into a
// factored/global-brightness mode (see docs/protocol.md); keep levels in 0–127.
// The agent only re-pushes a CHANGED report, so prime with -setAllLEDs: once after
// connecting if a stale agent cache makes lone single-channel writes not appear.

/// Set one channel in the local LED buffer (does not send until -flushLEDs).
- (void)setLED:(NSUInteger)channel level:(uint8_t)level;

/// Set every channel in the local LED buffer.
- (void)setAllLEDs:(uint8_t)level;

/// Set one RGB pad (0–15, in LED order) to a colour. Components are 0–127 (0 = off).
/// Does not send until -flushLEDs.
- (void)setPadLED:(NSUInteger)pad red:(uint8_t)r green:(uint8_t)g blue:(uint8_t)b;

/// Set one RGB group button (0–7 = A–H) to a colour. Components 0–127 (0 = off).
- (void)setGroupLED:(NSUInteger)group red:(uint8_t)r green:(uint8_t)g blue:(uint8_t)b;

/// Light the bottom `segments` (0–16) of a VU meter, the rest off.
/// left = YES for the left meter, NO for the right.
- (void)setVU:(BOOL)left segments:(NSUInteger)segments;

/// Set the shared VU colour (affects BOTH meters): YES = blue (input-monitor
/// look), NO = white (normal output metering). Does not send until -flushLEDs.
- (void)setVUBlue:(BOOL)blue;

/// Light one jog-ring segment (0 = top, increasing clockwise; 0–10). Level 0–127.
/// The short top-left arc has no channel and can't be lit.
- (void)setJogRingSegment:(NSUInteger)segment level:(uint8_t)level;

/// Send the current 213-byte LED buffer to the device.
- (void)flushLEDs;

@end
