// Synthetic catalog, transport and publication tests. No third-party IPAs.
#import "VMLegacyStore.h"
#import "VMLegacyStoreDownload.h"
#import "VMFirmwareFormats.h"
#include <assert.h>

static NSData *payload;
static NSString *payloadHash;
static NSString *mode;
static unsigned requests;
static NSMutableDictionary *row(void) {
    return [@{@"ipa_id":@123, @"bundle_id":@"org.example.Sample", @"name":@"Sample", @"developer":@"Example",
        @"version":@"1.0", @"size":@(payload.length), @"sha1":payloadHash,
        @"download_url":@"https://legacystore.app/ipa/123",
        @"icon_url":@"https://legacystore.app/icon/0000000000000000000000000000000000000000000000000000000000000000",
        @"compat":@{@"compatible":@YES, @"reasons":@[], @"architectures":@[@"armv6"],
            @"device_family":@[@"1"], @"armv7_code":@NO, @"min_os":@"3.1", @"install_status":@"installable"}} mutableCopy];
}
static NSData *json(id object) { return [NSJSONSerialization dataWithJSONObject:object options:0 error:NULL]; }
static NSData *catalog(NSArray *rows, NSString *version) {
    return json(@{@"target":@{@"model":@"iPhone1,2", @"os":version}, @"apps":rows});
}
static NSArray *parse(NSArray *rows) {
    return [VMLegacyStoreApp appsFromData:catalog(rows, @"3.1.3") target:VMLegacyStoreTarget.supportedGuest error:NULL];
}
static NSMutableDictionary *versionRow(void) {
    return [@{@"version":@"1.0", @"minimum_os_version":@"3.0", @"device_family":@[@1],
        @"copies":@[@{@"ipa_id":@"123", @"size":@(payload.length), @"sha1":payloadHash,
            @"install_status":@"installable", @"architectures":@[@"armv6"], @"macho_min_os":@"3.0",
            @"download_url":@"https://legacystore.app/ipa/123"}]} mutableCopy];
}
static NSArray<VMLegacyStoreVersion *> *parseVersions(NSArray *rows) {
    return [VMLegacyStoreVersion versionsFromData:json(@{@"data":rows, @"next_url":NSNull.null})
        target:VMLegacyStoreTarget.supportedGuest error:NULL];
}
static void versionMetadataTests(void) {
    assert(parseVersions(@[versionRow(), versionRow()]).count == 1);
    assert(parseVersions(@[versionRow()]).firstObject.copyIdentifiers.count == 1);
    assert(VMLegacyStoreAllowsURL([NSURL URLWithString:@"https://legacystore.app/api/v1/apps/org.example.Sample/versions"], VMLegacyStoreVersionsRequest));
    for (NSString *URL in @[@"https://legacystore.app/api/v1/apps/../versions", @"https://legacystore.app/api/v1/apps/id/versions?x=1",
        @"https://evil.invalid/api/v1/apps/123/versions", @"https://legacystore.app/api/v1/copies/123"]) {
        assert(!VMLegacyStoreAllowsURL([NSURL URLWithString:URL], VMLegacyStoreVersionsRequest));
    }
    for (NSDictionary *change in @[@{@"minimum_os_version":@"4.0"}, @{@"minimum_os_version":NSNull.null},
        @{@"device_family":@[@2]}, @{@"device_family":@"1"}, @{@"copies":@{}}, @{@"version":NSNull.null}]) {
        NSMutableDictionary *item = versionRow(); [item addEntriesFromDictionary:change]; assert(!parseVersions(@[item]).count);
    }
    for (NSDictionary *change in @[@{@"install_status":@"encrypted"}, @{@"architectures":@[@"armv7"]},
        @{@"macho_min_os":@"4.0"}, @{@"ipa_id":@"../123"}, @{@"size":@YES}, @{@"sha1":@"bad"},
        @{@"download_url":@"https://legacystore.app/ipa/124"}]) {
        NSMutableDictionary *item = versionRow(), *copy = [item[@"copies"][0] mutableCopy];
        [copy addEntriesFromDictionary:change]; item[@"copies"] = @[copy]; assert(!parseVersions(@[item]).count);
    }
    NSMutableDictionary *newer = versionRow(); newer[@"version"] = @"1.10";
    NSMutableDictionary *older = versionRow(); older[@"version"] = @"1.2";
    assert([[parseVersions(@[older,newer]).firstObject version] isEqual:@"1.10"]);
    NSError *error = nil;
    assert(![VMLegacyStoreVersion versionsFromData:json(@{@"data":@[], @"next_url":@"unexpected"}) target:VMLegacyStoreTarget.supportedGuest error:&error] && error);
}
static void metadataTests(void) {
    VMLegacyStoreTarget *target = VMLegacyStoreTarget.supportedGuest;
    NSURLComponents *URL = [NSURLComponents componentsWithURL:[target catalogURLForQuery:@"a&os=99"] resolvingAgainstBaseURL:NO];
    assert([URL.queryItems[0].value isEqual:@"iPhone1,2"] && [URL.queryItems[1].value isEqual:@"3.1.3"]);
    assert([URL.queryItems.lastObject.value isEqual:@"a&os=99"]);
    assert(parse(@[row()]).count == 1);
    assert(![VMLegacyStoreApp appsFromData:NSData.data target:target error:NULL]);
    assert(parse(@[row(), row()]).count == 1);
    NSMutableDictionary *stringID = row(); stringID[@"ipa_id"] = @"123";
    assert(parse(@[stringID]).count == 1);
    for (NSDictionary *change in @[@{@"ipa_id":@YES}, @{@"ipa_id":@(-1)}, @{@"ipa_id":@1.25},
        @{@"ipa_id":@"../123"}, @{@"name":NSNull.null}, @{@"bundle_id":@[]}, @{@"version":@{}},
        @{@"size":@YES}, @{@"size":@0}, @{@"size":@(-1)}, @{@"size":@1.5}, @{@"size":@(513ull * 1024 * 1024)},
        @{@"size":@"100"}, @{@"sha1":NSNull.null}, @{@"sha1":@"invalid"}, @{@"compat":@[]},
        @{@"download_url":@"https://legacystore.app/ipa/124"},
        @{@"download_url":@"https://archive.org/download/example/123.ipa"},
        @{@"download_url":@"http://legacystore.app/ipa/123"}]) {
        NSMutableDictionary *item = row(); [item addEntriesFromDictionary:change]; assert(parse(@[item]).count == 0);
    }
    for (NSDictionary *change in @[@{@"compatible":@NO}, @{@"compatible":@1}, @{@"reasons":@[@"encrypted"]},
        @{@"reasons":NSNull.null}, @{@"architectures":@[@"armv7"]}, @{@"architectures":@"armv6"},
        @{@"device_family":@[@"2"]}, @{@"armv7_code":@YES}, @{@"min_os":@"3.2"}, @{@"min_os":@"6.0"},
        @{@"min_os":@"3.1.3.1"}, @{@"min_os":@"3.bad"}, @{@"min_os":NSNull.null},
        @{@"install_status":@"encrypted"}, @{@"install_status":NSNull.null}]) {
        NSMutableDictionary *item = row(), *compat = [item[@"compat"] mutableCopy];
        [compat addEntriesFromDictionary:change]; item[@"compat"] = compat; assert(parse(@[item]).count == 0);
    }
    VMLegacyStoreTarget *older = [[VMLegacyStoreTarget alloc] initWithModel:@"iPhone1,2" OSVersion:@"3.0" architectures:@[@"armv6"] family:@"1"];
    assert([VMLegacyStoreApp appsFromData:catalog(@[row()], @"3.0") target:older error:NULL].count == 0);
    assert(![VMLegacyStoreApp appsFromData:catalog(@[row()], @"3.0") target:target error:NULL]);
    for (id bad in @[@[], @{}, @{@"target":NSNull.null, @"apps":@[]}, @{@"target":@{}, @"apps":@{}}])
        assert(![VMLegacyStoreApp appsFromData:json(bad) target:target error:NULL]);
    for (NSString *bad in @[@"http://archive.org/x", @"https://archive.org.example.com/x", @"https://archive.org:444/x",
        @"https://name:secret@archive.org/x", @"file:///tmp/x", @"https://legacystore.app/ipa/../123",
        @"https://legacystore.app/ipa/123?next=elsewhere", @"https://legacystore.app/ipa/123#fragment"]) {
        assert(!VMLegacyStoreAllowsURL([NSURL URLWithString:bad], VMLegacyStoreArchiveRequest));
    }
    assert(VMLegacyStoreAllowsURL([NSURL URLWithString:@"https://ia600001.us.archive.org/1/items/example/a.ipa"], VMLegacyStoreArchiveRequest));
    assert(!VMLegacyStoreAllowsURL([NSURL URLWithString:@"https://archive.org/icon/abc"], VMLegacyStoreIconRequest));
    assert(!VMLegacyStoreAllowsURL([NSURL URLWithString:@"https://legacystore.app/api/v1"], VMLegacyStoreCatalogRequest));
}

@interface StoreProtocol : NSURLProtocol
@property (atomic) BOOL stopped;
@end
@implementation StoreProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { (void)request; return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    NSString *fixture;
    @synchronized(StoreProtocol.class) { fixture = mode; requests++; }
    if ([fixture isEqual:@"network"]) {
        [self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorTimedOut userInfo:nil]]; return;
    }
    BOOL info = [self.request.URL.path hasPrefix:@"/api/"];
    NSData *data = info ? catalog(@[row()], @"3.1.3") : payload;
    if ([self.request.URL.path hasSuffix:@"/versions"]) data = json(@{@"data":@[versionRow()], @"next_url":NSNull.null});
    if ([fixture isEqual:@"wrong-app"]) {
        NSMutableDictionary *wrong = row(); wrong[@"bundle_id"] = @"org.example.Other"; data = catalog(@[wrong], @"3.1.3");
    }
    if ([fixture isEqual:@"large-catalog"]) data = [NSMutableData dataWithLength:2 * 1024 * 1024 + 1];
    NSMutableDictionary *headers = [@{@"Content-Length":@(data.length).stringValue} mutableCopy];
    if ([fixture isEqual:@"unknown"] || [fixture isEqual:@"large-catalog"]) [headers removeObjectForKey:@"Content-Length"];
    if ([fixture isEqual:@"length"]) headers[@"Content-Length"] = @(data.length + 1).stringValue;
    if ([fixture isEqual:@"short"]) data = [data subdataWithRange:NSMakeRange(0, data.length - 1)];
    if ([fixture isEqual:@"extra"]) { NSMutableData *extra = [data mutableCopy]; [extra appendBytes:"x" length:1]; data = extra; }
    if ([fixture isEqual:@"hash"]) { NSMutableData *changed = [data mutableCopy]; ((uint8_t *)changed.mutableBytes)[0] ^= 1; data = changed; }
    NSInteger status = [fixture isEqual:@"http"] ? 503 : [fixture isEqual:@"rate"] ? 429 : 200;
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:status HTTPVersion:@"HTTP/1.1" headerFields:headers];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if ([fixture isEqual:@"slow"]) [NSThread sleepForTimeInterval:0.15];
    if (self.stopped) return;
    [self.client URLProtocol:self didLoadData:data]; [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading { self.stopped = YES; }
@end
static void pump(BOOL (^done)(void)) {
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:10];
    while (!done() && until.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(done());
}
static void setMode(NSString *value) { @synchronized(StoreProtocol.class) { mode = value; } }
static void downloadTests(NSURL *root, NSURLSessionConfiguration *configuration) {
    NSURL *directory = [root URLByAppendingPathComponent:@"downloads" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    NSURL *abandoned = [directory URLByAppendingPathComponent:[@"attempt-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    [NSFileManager.defaultManager createDirectoryAtURL:abandoned withIntermediateDirectories:NO attributes:nil error:NULL];
    NSURL *unrelated = [directory URLByAppendingPathComponent:@"keep.ipa"];
    assert([payload writeToURL:unrelated atomically:YES]);
    __block unsigned publications = 0;
    __block BOOL refuse = NO;
    VMLegacyStoreDownload *download = [[VMLegacyStoreDownload alloc] initWithDirectory:directory configuration:configuration
        publisher:^NSURL *(NSURL *file, VMLegacyStoreApp *app, NSError **error) {
            assert(!NSThread.isMainThread && [app.identifier isEqual:@"123"]);
            assert([[NSData dataWithContentsOfURL:file] isEqual:payload]);
            if (refuse) { *error = VMLegacyStoreError(@"Invalid IPA"); return nil; }
            publications++;
            NSURL *output = [root URLByAppendingPathComponent:[NSString stringWithFormat:@"published-%u.ipa", publications]];
            return [NSFileManager.defaultManager copyItemAtURL:file toURL:output error:error] ? output : nil;
        }];
    assert(![NSFileManager.defaultManager fileExistsAtPath:abandoned.path]);
    VMLegacyStoreApp *app = parse(@[row()]).firstObject;
    for (NSString *fixture in @[@"ok", @"unknown", @"http", @"rate", @"network", @"length", @"short", @"extra", @"hash", @"slow", @"invalid-ipa", @"ok"]) {
        setMode(fixture); refuse = [fixture isEqual:@"invalid-ipa"];
        unsigned before = publications;
        assert([download startApp:app]); assert(![download startApp:app]);
        if ([fixture isEqual:@"slow"]) [download cancel];
        pump(^BOOL { return !download.busy; });
        BOOL success = [fixture isEqual:@"ok"] || [fixture isEqual:@"unknown"];
        assert(download.state == (success ? VMLegacyStoreDownloaded : [fixture isEqual:@"slow"] ? VMLegacyStoreDownloadCancelled : VMLegacyStoreDownloadFailed));
        assert(publications == before + (success ? 1u : 0u));
        assert([NSFileManager.defaultManager contentsOfDirectoryAtURL:directory includingPropertiesForKeys:nil options:0 error:NULL].count == 1);
        assert([[NSData dataWithContentsOfURL:unrelated] isEqual:payload]);
    }
    // An existing destination is refused without truncating or deleting it.
    setMode(@"ok"); __block BOOL done = NO;
    VMLegacyStoreRequest *request = [[VMLegacyStoreRequest alloc] initWithURL:app.downloadURL kind:VMLegacyStoreArchiveRequest
        configuration:configuration destination:unrelated expectedSize:app.size SHA1:app.SHA1 progress:nil
        completion:^(NSData *data, NSError *error) { (void)data; assert(error); done = YES; }];
    pump(^BOOL { return done; }); (void)request;
    assert([[NSData dataWithContentsOfURL:unrelated] isEqual:payload]);
}
static void catalogTests(NSURLSessionConfiguration *configuration) {
    VMLegacyStoreCatalog *client = [[VMLegacyStoreCatalog alloc] initWithConfiguration:configuration];
    setMode(@"ok"); __block unsigned completions = 0;
    void (^completion)(NSArray *, BOOL, NSError *) = ^(NSArray *apps, BOOL cached, NSError *error) {
        assert(NSThread.isMainThread && apps.count == 1 && !cached && !error); completions++;
    };
    [client search:@"test" target:VMLegacyStoreTarget.supportedGuest completion:completion];
    pump(^BOOL { return completions == 1; });
    unsigned before; @synchronized(StoreProtocol.class) { before = requests; }
    [client search:@"test" target:VMLegacyStoreTarget.supportedGuest completion:completion];
    assert(completions == 2);
    @synchronized(StoreProtocol.class) { assert(requests == before); }
    setMode(@"slow");
    [client search:@"stale" target:VMLegacyStoreTarget.supportedGuest completion:^(NSArray *apps, BOOL cached, NSError *error) {
        (void)apps; (void)cached; (void)error; assert(!"Cancelled search must not replace a newer query");
    }];
    [client search:@"latest" target:VMLegacyStoreTarget.supportedGuest completion:completion];
    pump(^BOOL { return completions == 3; });
    setMode(@"large-catalog"); __block BOOL failed = NO;
    [client search:@"too large" target:VMLegacyStoreTarget.supportedGuest completion:^(NSArray *apps, BOOL cached, NSError *error) {
        assert(!apps.count && !cached && error); failed = YES;
    }];
    pump(^BOOL { return failed; });
    @synchronized(StoreProtocol.class) { before = requests; }
    failed = NO;
    [client search:@"cooldown" target:VMLegacyStoreTarget.supportedGuest completion:^(NSArray *apps, BOOL cached, NSError *error) {
        assert(!apps.count && !cached && error); failed = YES;
    }];
    assert(failed); @synchronized(StoreProtocol.class) { assert(requests == before); }
}
static void versionRequestTests(NSURLSessionConfiguration *configuration) {
    VMLegacyStoreVersions *client = [[VMLegacyStoreVersions alloc] initWithConfiguration:configuration];
    VMLegacyStoreApp *app = parse(@[row()]).firstObject;
    setMode(@"ok"); __block BOOL done = NO; __block VMLegacyStoreVersion *selected;
    [client loadApp:app completion:^(NSArray *versions, NSError *error) {
        assert(!error && versions.count == 1); selected = versions.firstObject; done = YES;
    }];
    pump(^BOOL { return done; }); done = NO;
    [client resolveVersion:selected app:app completion:^(VMLegacyStoreApp *copy, NSError *error) {
        assert(!error && [copy.identifier isEqual:@"123"]); done = YES;
    }];
    pump(^BOOL { return done; }); done = NO; setMode(@"wrong-app");
    [client resolveVersion:selected app:app completion:^(VMLegacyStoreApp *copy, NSError *error) {
        assert(!copy && error); done = YES;
    }];
    pump(^BOOL { return done; }); setMode(@"slow");
    [client loadApp:app completion:^(NSArray *versions, NSError *error) { (void)versions; (void)error; assert(!"stale versions callback"); }];
    [client cancel]; setMode(@"ok"); done = NO;
    [client loadApp:app completion:^(NSArray *versions, NSError *error) { assert(versions.count && !error); done = YES; }];
    pump(^BOOL { return done; });
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        payload = [@"synthetic archive bytes for transport tests" dataUsingEncoding:NSUTF8StringEncoding];
        uint8_t digest[20]; assert(vmfw_sha1(payload.bytes, payload.length, digest));
        NSMutableString *hex = [NSMutableString string];
        for (unsigned i = 0; i < sizeof digest; i++) [hex appendFormat:@"%02x", digest[i]];
        payloadHash = hex;
        metadataTests(); versionMetadataTests();
        NSURL *root = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]
            URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        assert([NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:NULL]);
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.protocolClasses = @[StoreProtocol.class];
        downloadTests(root, config); catalogTests(config); versionRequestTests(config);
        assert([NSFileManager.defaultManager removeItemAtURL:root error:NULL]);
        puts("Legacy Store metadata, guest filtering, bounded transport, cancellation, cache and publication tests passed");
    }
    return 0;
}
