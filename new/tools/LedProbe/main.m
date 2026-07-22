//
//  main.m
//  MacchinaLedProbe — NewClients/LedProbe
//
//  Interactive LED + input mapper for the Maschine Studio. It lights ONE LED
//  channel at a time; for each, you type what physical LED it is, and — if it's
//  a button/pad — you PRESS that control so the tool also captures its input id.
//  Every channel becomes one row: channel · your label · captured input ids.
//  The map is appended to a TSV file (flushed per row) that can be read back.
//
//  Build:  ./build.sh ledprobe
//  Run:    ./build/MacchinaLedProbe [serial] [outfile.tsv]
//          (NIHardwareAgent must be running; defaults: serial 39195855,
//           outfile ./led_map.tsv)
//
//  Per channel: press the control (if any), type its name, press Enter.
//    <name>     record this channel with that label + captured ids, go next
//    <Enter>    skip (records label "(skip)"), go next
//    p          go back one channel (re-do it)
//    q          quit
//
//  Lit channel = 127; all others = 1 (off; 0 means "no change"). State is
//  re-sent at 30Hz because one-shot LED writes aren't reliably pushed.
//

#import <Foundation/Foundation.h>
#import "NIStudioController.h"

// Collects the UNIQUE input controls touched since the last reset, so a stream
// of pad-pressure / knob-delta events collapses to one id each.
@interface ProbeDelegate : NSObject <NIStudioControllerDelegate>
@property (nonatomic) NSMutableArray<NSString *> * captured;
@end

@implementation ProbeDelegate

- (instancetype)init { if ((self = [super init])) _captured = [NSMutableArray array]; return self; }

- (void)note:(NSString *)what
{
    // Called on the main run loop (same as command processing) — no lock needed.
    if (![self.captured containsObject:what])
    {
        [self.captured addObject:what];
        printf("   [captured: %s]\n> ", what.UTF8String);
        fflush(stdout);
    }
}

- (void)studioController:(NIStudioController *)c padEvent:(NIStudioPadEvent)e
{ [self note:[NSString stringWithFormat:@"pad %u", e.padId]]; }

- (void)studioController:(NIStudioController *)c buttonId:(uint32_t)b down:(BOOL)d
{ if (d) [self note:[NSString stringWithFormat:@"button 0x%02x", b]]; }

- (void)studioController:(NIStudioController *)c knobId:(uint32_t)k delta:(float)d
{ [self note:[NSString stringWithFormat:@"knob %u", k]]; }

- (void)studioController:(NIStudioController *)c wheelId:(uint32_t)w step:(int32_t)s
{ [self note:[NSString stringWithFormat:@"wheel %u", w]]; }

@end


int main(int argc, const char * argv[])
{
    @autoreleasepool
    {
        NSString * serial  = (argc > 1) ? @(argv[1]) : @"39195855";
        NSString * outPath = (argc > 2) ? @(argv[2]) : @"led_map.tsv";

        NIStudioController * studio = [NIStudioController studioWithSerial:serial];
        ProbeDelegate * probe = [ProbeDelegate new];
        studio.delegate = probe;

        printf("connecting to Maschine Studio (serial %s)…\n", serial.UTF8String);
        if ([studio connect] == NO)
        {
            fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
            return 1;
        }

        FILE * out = fopen(outPath.fileSystemRepresentation, "a");
        if (out == NULL) { perror("open outfile"); return 1; }
        fprintf(out, "# channel\tlabel\tcaptured_ids\n");
        fflush(out);
        char abspathBuf[PATH_MAX];
        NSString * abspath = realpath(outPath.fileSystemRepresentation, abspathBuf)
                           ? @(abspathBuf) : outPath;
        printf("writing map to %s\n", abspath.UTF8String);

        // Continuously re-send: lit channel at 127, everything else at 1 (off).
        __block volatile int cursor = 0;
        dispatch_queue_t q = dispatch_queue_create("macchina.ledprobe", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(timer, ^{
            int c = cursor;
            [studio setAllLEDs:1];
            [studio setLED:(NSUInteger)c level:127];
            [studio flushLEDs];
        });
        dispatch_resume(timer);

        void (^prompt)(void) = ^{
            printf("\n→ channel %d lit. Press its control (if any), type a name, Enter"
                   "  [empty=skip, p=back, q=quit]\n> ", cursor);
            fflush(stdout);
        };

        printf("\nInteractive LED + input mapper. %lu channels.\n",
               (unsigned long)NIStudioLEDChannelCount);
        prompt();

        // Read stdin on a background thread so the main run loop keeps processing
        // input-event frames (needed to capture button/pad ids).
        void (^process)(NSString *) = ^(NSString * raw){
            NSString * line = [raw stringByTrimmingCharactersInSet:
                               [NSCharacterSet whitespaceAndNewlineCharacterSet]];

            if ([line isEqualToString:@"q"])
            {
                fclose(out);
                dispatch_sync(q, ^{ [studio setAllLEDs:1]; [studio flushLEDs]; });
                printf("\nsaved %s — bye.\n", abspath.UTF8String); fflush(stdout);
                exit(0);
            }
            if ([line isEqualToString:@"p"])
            {
                if (cursor > 0) cursor--;
                [probe.captured removeAllObjects];
                prompt();
                return;
            }

            NSString * label = line.length ? line : @"(skip)";
            NSString * ids = [probe.captured componentsJoinedByString:@","];
            fprintf(out, "%d\t%s\t%s\n", cursor, label.UTF8String, ids.UTF8String);
            fflush(out);
            printf("  recorded: ch %d = \"%s\"%s%s\n", cursor, label.UTF8String,
                   ids.length ? "  ids: " : "", ids.UTF8String);

            [probe.captured removeAllObjects];
            if (cursor < (int)NIStudioLEDChannelCount - 1) cursor++;
            prompt();
        };

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            char buf[256];
            while (fgets(buf, sizeof(buf), stdin) != NULL)
            {
                NSString * s = @(buf);
                dispatch_async(dispatch_get_main_queue(), ^{ process(s); });
            }
            // EOF (Ctrl-D): quit cleanly.
            dispatch_async(dispatch_get_main_queue(), ^{ process(@"q"); });
        });

        [[NSRunLoop currentRunLoop] run];
    }
    return 0;
}
