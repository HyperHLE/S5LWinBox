// Foundation transport/lifecycle tests. All responses and archive bytes are
// synthetic; this suite never downloads or contains Apple firmware.
#import "VMFirmwareDownloader.h"
#import "VMFirmwareImporter.h"
#include "sha256.h"
#include <assert.h>

static NSData *payload;
static NSString *fixtureMode;
static NSString *archiveHash;
static NSString *const AppleURL = @"https://secure-appldnld.apple.com/test.ipsw";
static NSMutableDictionary *metadata(void) {
    return [@{@"identifier":@"iPhone1,2", @"version":@"3.1.3", @"buildid":@"7E18",
        @"sha256sum":archiveHash, @"filesize":@(payload.length), @"url":AppleURL} mutableCopy];
}
static NSData *json(id value) { return [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL]; }

@interface DownloadProtocol : NSURLProtocol
@property (atomic) BOOL stopped;
@end
@implementation DownloadProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { (void)request; return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    NSString *mode;
    @synchronized(DownloadProtocol.class) { mode = fixtureMode; }
    if ([mode isEqual:@"network-failure"]) {
        [self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorTimedOut userInfo:nil]];
        return;
    }
    BOOL info = [self.request.URL.host isEqual:@"api.ipsw.me"];
    NSMutableDictionary *record = metadata();
    if ([mode isEqual:@"wrong-device"]) record[@"identifier"] = @"iPhone1,1";
    if ([mode isEqual:@"bad-url"]) record[@"url"] = @"https://example.com/firmware.ipsw";
    if ([mode isEqual:@"bad-checksum"]) record[@"sha256sum"] = [@"" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0];
    NSData *body = info ? json(record) : payload;
    if (info && [mode isEqual:@"large-metadata"]) body = [NSMutableData dataWithLength:65537];
    NSMutableDictionary *headers = [@{@"Content-Length":@(body.length).stringValue} mutableCopy];
    if ([mode isEqual:@"unknown-length"]) [headers removeObjectForKey:@"Content-Length"];
    if (!info && [mode isEqual:@"wrong-length"]) headers[@"Content-Length"] = @(body.length + 1).stringValue;
    if (!info && [mode isEqual:@"short"]) body = [body subdataWithRange:NSMakeRange(0, body.length - 1)];
    if (!info && [mode isEqual:@"oversize"]) {
        NSMutableData *extra = [body mutableCopy]; [extra appendBytes:"x" length:1]; body = extra;
    }
    NSInteger code = [mode isEqual:@"http-failure"] ? 503 : 200;
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:code HTTPVersion:@"HTTP/1.1" headerFields:headers];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if (!info) [NSThread sleepForTimeInterval:0.04]; // Let cancellation reach the serial delegate.
    if (self.stopped) return;
    [self.client URLProtocol:self didLoadData:body];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading { self.stopped = YES; }
@end

@interface DownloadImporter : VMFirmwareImporter
@property (nonatomic, strong) NSURL *chosen;
@property BOOL busy;
@property BOOL cancelled;
@property BOOL fail;
@property unsigned imports;
@end
@implementation DownloadImporter
- (BOOL)isRunning { return self.busy; }
- (NSURL *)selectedURL { return self.chosen; }
- (void)importIPSWAtURL:(NSURL *)url {
    assert(NSThread.isMainThread && !self.busy);
    assert([[NSData dataWithContentsOfURL:url] isEqual:payload]);
    self.chosen = url; self.busy = YES; self.cancelled = NO; self.imports++;
    [NSNotificationCenter.defaultCenter postNotificationName:VMFirmwareImportDidChangeNotification object:self];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        self.busy = NO;
        [NSNotificationCenter.defaultCenter postNotificationName:VMFirmwareImportDidChangeNotification object:self];
    });
}
- (void)cancelImport { self.cancelled = YES; }
- (BOOL)getLastReport:(vm_fw_report_t *)report {
    memset(report, 0, sizeof *report);
    report->status = self.cancelled ? VM_FW_ERR_CANCELLED : self.fail ? VM_FW_ERR_VERIFICATION : VM_FW_OK;
    report->manifest_read = report->reference_build = true;
    for (unsigned i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
        report->artefacts[i].state = VM_FW_STATE_VERIFIED;
        report->artefacts[i].sha256_valid = report->artefacts[i].reference_known = true;
        report->artefacts[i].matches_reference = true;
        report->artefacts[i].produced = 1;
    }
    return YES;
}
- (void)forgetSelectedURL:(NSURL *)url { if (!self.busy && [self.chosen isEqual:url]) self.chosen = nil; }
@end

static void pumpUntil(BOOL (^done)(void)) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:10];
    while (!done() && end.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(done());
}
static void setMode(NSString *mode) { @synchronized(DownloadProtocol.class) { fixtureMode = mode; } }
static void checkMetadata(void) {
    assert([VMFirmwareDownloadMetadata metadataFromData:json(metadata()) error:NULL]);
    for (NSDictionary *change in @[
        @{@"identifier":@"iPhone2,1"}, @{@"version":@"6.1.6"}, @{@"buildid":@"other"},
        @{@"sha256sum":@"bad"}, @{@"sha256sum":NSNull.null}, @{@"filesize":@0},
        @{@"filesize":@YES}, @{@"filesize":@(-1)}, @{@"filesize":@1.5},
        @{@"filesize":@(1024ull * 1024 * 1024)}, @{@"filesize":@"239139281"},
        @{@"url":@"http://secure-appldnld.apple.com/test.ipsw"},
        @{@"url":@"https://secure-appldnld.apple.com.example.com/test.ipsw"},
        @{@"url":@"https://user:password@secure-appldnld.apple.com/test.ipsw"},
        @{@"url":@"https://secure-appldnld.apple.com:444/test.ipsw"},
        @{@"url":@"file:///tmp/test.ipsw"}
    ]) {
        NSMutableDictionary *value = metadata(); [value addEntriesFromDictionary:change];
        NSError *error = nil;
        assert(![VMFirmwareDownloadMetadata metadataFromData:json(value) error:&error] && error);
    }
    assert(![VMFirmwareDownloadMetadata metadataFromData:json(@[]) error:NULL]);
    assert(![VMFirmwareDownloadMetadata metadataFromData:[NSMutableData dataWithLength:65537] error:NULL]);
    assert(![VMFirmwareDownloadMetadata allowsURL:[NSURL URLWithString:@"https://ipsw.me/file"] metadata:YES]);
    assert(![VMFirmwareDownloadMetadata allowsURL:[NSURL URLWithString:AppleURL] metadata:YES]);
    assert(![VMFirmwareDownloadMetadata allowsURL:[NSURL URLWithString:@"https://api.ipsw.me/file"] metadata:NO]);
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        payload = [@"Synthetic archive fixture; not firmware." dataUsingEncoding:NSUTF8StringEncoding];
        uint8_t digest[32]; assert(ios3_sha256(payload.bytes, payload.length, digest));
        NSMutableString *hash = [NSMutableString new];
        for (unsigned i = 0; i < 32; i++) [hash appendFormat:@"%02x", digest[i]];
        archiveHash = hash;
        checkMetadata();
        NSFileManager *fm = NSFileManager.defaultManager;
        NSURL *root = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]
            URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        assert([fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:NULL]);
        NSURL *manual = [root URLByAppendingPathComponent:@"manual.ipsw"];
        assert([payload writeToURL:manual atomically:YES]);
        for (NSString *mode in @[@"success", @"unknown-length", @"wrong-device", @"bad-url", @"bad-checksum",
            @"large-metadata", @"http-failure", @"network-failure", @"wrong-length", @"short", @"oversize", @"import-failure", @"cancel-metadata",
            @"cancel-download", @"cancel-import", @"manual-handoff"]) {
            setMode(mode);
            NSURL *cache = [root URLByAppendingPathComponent:mode isDirectory:YES];
            NSURL *orphan = [cache URLByAppendingPathComponent:[@"attempt-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
            assert([fm createDirectoryAtURL:orphan withIntermediateDirectories:YES attributes:nil error:NULL]);
            assert([payload writeToURL:[orphan URLByAppendingPathComponent:@"partial"] atomically:YES]);
            NSURL *untouched = [cache URLByAppendingPathComponent:@"unrelated.txt"];
            assert([payload writeToURL:untouched atomically:YES]);
            DownloadImporter *importer = [DownloadImporter new]; importer.fail = [mode isEqual:@"import-failure"];
            NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
            configuration.protocolClasses = @[DownloadProtocol.class];
            VMFirmwareDownloader *downloader = [[VMFirmwareDownloader alloc] initWithImporter:importer cacheDirectory:cache configuration:configuration];
            id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMFirmwareDownloadDidChangeNotification
                object:downloader queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *note) {
                    assert(NSThread.isMainThread);
                    if ([mode isEqual:@"cancel-download"] && downloader.phase == VMFirmwareDownloadReceiving) [downloader cancel];
                    if ([mode isEqual:@"manual-handoff"] && downloader.phase == VMFirmwareDownloadImporting && !importer.busy) {
                        importer.chosen = manual; importer.busy = YES;
                        [downloader cancel]; // Must not cancel or forget this unrelated manual import.
                    }
                }];
            [downloader start]; [downloader start]; // No second concurrent operation.
            if ([mode isEqual:@"cancel-metadata"]) [downloader cancel];
            if ([mode isEqual:@"cancel-import"]) { pumpUntil(^BOOL { return importer.busy || !downloader.running; }); assert(importer.busy); [downloader cancel]; }
            pumpUntil(^BOOL { return !downloader.running; });
            BOOL success = [@[@"success", @"unknown-length"] containsObject:mode];
            BOOL cancelled = [mode hasPrefix:@"cancel-"] || [mode isEqual:@"manual-handoff"];
            assert(downloader.phase == (success ? VMFirmwareDownloadComplete : cancelled ? VMFirmwareDownloadCancelled : VMFirmwareDownloadFailed));
            assert(success || cancelled || downloader.error);
            assert(importer.imports == ((success || [@[@"import-failure", @"cancel-import"] containsObject:mode]) ? 1u : 0u));
            assert(!importer.chosen || ([mode isEqual:@"manual-handoff"] && [importer.chosen isEqual:manual] && !importer.cancelled));
            assert(![fm fileExistsAtPath:orphan.path]);
            assert([[fm contentsOfDirectoryAtPath:cache.path error:NULL] isEqual:@[@"unrelated.txt"]]);
            assert([[NSData dataWithContentsOfURL:manual] isEqual:payload]);
            [NSNotificationCenter.defaultCenter removeObserver:observer];
            if ([mode isEqual:@"bad-checksum"]) {
                setMode(@"success"); [downloader start]; pumpUntil(^BOOL { return !downloader.running; });
                assert(downloader.phase == VMFirmwareDownloadComplete && importer.imports == 1);
            }
            printf("PASS %s\n", mode.UTF8String);
        }
        assert([fm removeItemAtURL:root error:NULL]);
        puts("Firmware download metadata, streaming, checksum, cancellation, handoff and cleanup passed.");
    }
    return 0;
}
