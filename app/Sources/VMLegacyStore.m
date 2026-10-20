#import "VMLegacyStore.h"
#import "VMFirmwareFormats.h"
#include <math.h>

NSError *VMLegacyStoreError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.LegacyStore" code:1
        userInfo:@{NSLocalizedDescriptionKey:message}];
}
static BOOL Text(id value, NSUInteger limit) {
    return [value isKindOfClass:NSString.class] && [value length] > 0 && [value length] <= limit &&
        [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound;
}
static BOOL Digits(NSString *value, NSUInteger limit) {
    return Text(value, limit) && [value rangeOfCharacterFromSet:
        [NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet].location == NSNotFound;
}
static BOOL Version(id value, unsigned *out) {
    if (!Text(value, 11)) return NO;
    NSArray *parts = [value componentsSeparatedByString:@"."];
    if (parts.count > 3) return NO;
    unsigned result = 0;
    for (NSUInteger i = 0; i < 3; i++) {
        NSString *part = i < parts.count ? parts[i] : @"0";
        if (!Digits(part, 3) || part.intValue > 255) return NO;
        result = (result << 8) | part.intValue;
    }
    if (out) *out = result;
    return YES;
}
static BOOL Hex(id value, NSUInteger length) {
    return Text(value, length) && [value length] == length && [value rangeOfCharacterFromSet:
        [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"].invertedSet].location == NSNotFound;
}
static BOOL Integer(id value, uint64_t maximum) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return NO;
    double number = [value doubleValue];
    return isfinite(number) && number >= 1 && number <= maximum && floor(number) == number;
}
BOOL VMLegacyStoreAllowsURL(NSURL *URL, VMLegacyStoreRequestKind kind) {
    if (![URL.scheme.lowercaseString isEqual:@"https"] || URL.user || URL.password ||
        (URL.port && URL.port.integerValue != 443) || URL.fragment) return NO;
    NSString *host = URL.host.lowercaseString;
    if (kind == VMLegacyStoreArchiveRequest &&
        ([host isEqual:@"archive.org"] || [host hasSuffix:@".archive.org"])) return YES;
    if (![host isEqual:@"legacystore.app"]) return NO;
    if (kind == VMLegacyStoreCatalogRequest) return [URL.path isEqual:@"/api/emulator/apps"];
    if (URL.query) return NO;
    if (kind == VMLegacyStoreVersionsRequest) {
        NSArray *parts = URL.path.pathComponents;
        NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"];
        return parts.count == 6 && [parts[1] isEqual:@"api"] && [parts[2] isEqual:@"v1"] &&
            [parts[3] isEqual:@"apps"] && Text(parts[4], 200) &&
            ![@[@".", @".."] containsObject:parts[4]] &&
            [parts[4] rangeOfCharacterFromSet:allowed.invertedSet].location == NSNotFound && [parts[5] isEqual:@"versions"];
    }
    if (kind == VMLegacyStoreIconRequest)
        return [URL.path hasPrefix:@"/icon/"] && Hex([URL.path substringFromIndex:6], 64);
    return [URL.path hasPrefix:@"/ipa/"] && Digits([URL.path substringFromIndex:5], 16);
}

@implementation VMLegacyStoreTarget
+ (instancetype)supportedGuest {
    // The compiled boot path currently supports only this profile.
    return [[self alloc] initWithModel:@"iPhone1,2" OSVersion:@"3.1.3" architectures:@[@"armv6"] family:@"1"];
}
- (instancetype)initWithModel:(NSString *)model OSVersion:(NSString *)version
    architectures:(NSArray<NSString *> *)architectures family:(NSString *)family {
    self = [super init];
    if (!self || !Text(model, 32) || !Version(version, NULL) || !architectures.count ||
        ![@[@"1", @"2"] containsObject:family]) return nil;
    for (id arch in architectures) if (![@[@"armv6", @"armv7", @"armv7s", @"arm64"] containsObject:arch]) return nil;
    _model = [model copy]; _OSVersion = [version copy]; _architectures = [architectures copy]; _family = [family copy];
    return self;
}
- (NSURL *)catalogURLForQuery:(NSString *)query {
    NSURLComponents *URL = [NSURLComponents componentsWithString:@"https://legacystore.app/api/emulator/apps"];
    NSString *trimmed = [query stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length > 160) trimmed = [trimmed substringToIndex:160];
    URL.queryItems = @[[NSURLQueryItem queryItemWithName:@"device" value:self.model],
        [NSURLQueryItem queryItemWithName:@"os" value:self.OSVersion],
        [NSURLQueryItem queryItemWithName:@"family" value:self.family],
        [NSURLQueryItem queryItemWithName:@"limit" value:@"50"],
        [NSURLQueryItem queryItemWithName:@"q" value:trimmed]];
    return URL.URL;
}
- (NSURL *)catalogURLForCopy:(NSString *)identifier {
    NSURLComponents *URL = [NSURLComponents componentsWithURL:[self catalogURLForQuery:@""] resolvingAgainstBaseURL:NO];
    NSMutableArray *items = [URL.queryItems mutableCopy];
    [items addObject:[NSURLQueryItem queryItemWithName:@"ipa_id" value:identifier]];
    URL.queryItems = items; return URL.URL;
}
@end

@implementation VMLegacyStoreApp
+ (NSArray<VMLegacyStoreApp *> *)appsFromData:(NSData *)data target:(VMLegacyStoreTarget *)target error:(NSError **)error {
    id root = data.length > 0 && data.length <= 2 * 1024 * 1024 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    NSDictionary *identity = [root isKindOfClass:NSDictionary.class] ? root[@"target"] : nil;
    id rows = [root isKindOfClass:NSDictionary.class] ? root[@"apps"] : nil;
    if (![identity isKindOfClass:NSDictionary.class] || ![identity[@"model"] isEqual:target.model] ||
        ![identity[@"os"] isEqual:target.OSVersion] || ![rows isKindOfClass:NSArray.class] || [rows count] > 50) {
        if (error) *error = VMLegacyStoreError(@"Legacy Store returned an invalid catalog. Try again later.");
        return nil;
    }
    NSMutableArray *apps = [NSMutableArray array];
    NSMutableSet *seen = [NSMutableSet set];
    unsigned targetVersion = 0; Version(target.OSVersion, &targetVersion);
    for (id row in rows) {
        if (![row isKindOfClass:NSDictionary.class]) continue;
        id compat = row[@"compat"];
        if (![compat isKindOfClass:NSDictionary.class] || compat[@"compatible"] != (__bridge id)kCFBooleanTrue ||
            ![compat[@"install_status"] isEqual:@"installable"] ||
            ![compat[@"reasons"] isKindOfClass:NSArray.class] || [compat[@"reasons"] count]) continue;
        id archs = compat[@"architectures"], families = compat[@"device_family"];
        if (![archs isKindOfClass:NSArray.class] || ![families isKindOfClass:NSArray.class] ||
            ![families containsObject:target.family]) continue;
        BOOL architecture = NO;
        for (NSString *arch in target.architectures) if ([archs containsObject:arch]) architecture = YES;
        if (!architecture || (target.architectures.count == 1 && [target.architectures[0] isEqual:@"armv6"] &&
            compat[@"armv7_code"] == (__bridge id)kCFBooleanTrue)) continue;
        unsigned minimum = 0;
        // Missing/ambiguous minimum OS cannot produce a trustworthy automatic match.
        if (!Version(compat[@"min_os"], &minimum) || minimum > targetVersion) continue;
        id identifier = row[@"ipa_id"];
        if (Integer(identifier, 9007199254740991ULL)) identifier = [identifier stringValue];
        if (!Digits(identifier, 16) || [identifier longLongValue] <= 0 || [seen containsObject:identifier] ||
            !Text(row[@"name"], 300) || !Text(row[@"bundle_id"], 200) || !Text(row[@"version"], 80) ||
            !Integer(row[@"size"], 512ull * 1024 * 1024) || !Hex(row[@"sha1"], 40) ||
            !Text(row[@"download_url"], 1024)) continue;
        NSURL *download = [NSURL URLWithString:row[@"download_url"]];
        // Bind the copy ID to the canonical resolver, not a metadata-supplied arbitrary archive URL.
        if (!VMLegacyStoreAllowsURL(download, VMLegacyStoreArchiveRequest) ||
            ![download.absoluteString isEqual:[@"https://legacystore.app/ipa/" stringByAppendingString:identifier]]) continue;
        VMLegacyStoreApp *app = [self new];
        app->_identifier = [identifier copy]; app->_bundleIdentifier = [row[@"bundle_id"] copy];
        app->_name = [row[@"name"] copy]; app->_developer = Text(row[@"developer"], 200) ? [row[@"developer"] copy] : @"";
        app->_version = [row[@"version"] copy]; app->_minimumOS = [compat[@"min_os"] copy];
        app->_SHA1 = [row[@"sha1"] lowercaseString]; app->_size = [row[@"size"] unsignedLongLongValue];
        app->_downloadURL = download; app->_target = target;
        NSURL *icon = Text(row[@"icon_url"], 1024) ? [NSURL URLWithString:row[@"icon_url"]] : nil;
        if (VMLegacyStoreAllowsURL(icon, VMLegacyStoreIconRequest)) app->_iconURL = icon;
        [seen addObject:identifier]; [apps addObject:app];
    }
    return apps;
}
@end

@implementation VMLegacyStoreVersion
+ (NSArray<VMLegacyStoreVersion *> *)versionsFromData:(NSData *)data target:(VMLegacyStoreTarget *)target error:(NSError **)error {
    id root = data.length && data.length <= 2u * 1024u * 1024u ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    id rows = [root isKindOfClass:NSDictionary.class] ? root[@"data"] : nil;
    if (![rows isKindOfClass:NSArray.class] || [rows count] > 2000 ||
        (root[@"next_url"] && root[@"next_url"] != NSNull.null)) {
        if (error) *error = VMLegacyStoreError(@"The version list could not be read. Try again later."); return nil;
    }
    unsigned maximum = 0; Version(target.OSVersion, &maximum);
    NSMutableArray *versions = [NSMutableArray array];
    NSMutableDictionary *byVersion = [NSMutableDictionary dictionary];
    for (id row in rows) {
        if (![row isKindOfClass:NSDictionary.class] || !Text(row[@"version"], 80)) continue;
        unsigned minimum = 0;
        if (!Version(row[@"minimum_os_version"], &minimum) || minimum > maximum) continue;
        id families = row[@"device_family"];
        if (families != nil && families != NSNull.null && (![families isKindOfClass:NSArray.class] ||
            ([families count] && ![families containsObject:@(target.family.integerValue)] && ![families containsObject:target.family]))) continue;
        id copies = row[@"copies"];
        if (![copies isKindOfClass:NSArray.class] || [copies count] > 256) continue;
        NSMutableArray *ids = [NSMutableArray array];
        VMLegacyStoreVersion *existing = byVersion[row[@"version"]];
        if (existing) [ids addObjectsFromArray:existing.copyIdentifiers];
        for (id copy in copies) {
            if (![copy isKindOfClass:NSDictionary.class] || ![copy[@"install_status"] isEqual:@"installable"] ||
                ![copy[@"architectures"] isKindOfClass:NSArray.class] || !Digits(copy[@"ipa_id"], 16) ||
                [copy[@"ipa_id"] longLongValue] <= 0 || !Integer(copy[@"size"], 512ull * 1024 * 1024) || !Hex(copy[@"sha1"], 40) ||
                ![copy[@"download_url"] isEqual:[@"https://legacystore.app/ipa/" stringByAppendingString:copy[@"ipa_id"]]]) continue;
            BOOL architecture = NO;
            for (NSString *arch in target.architectures) if ([copy[@"architectures"] containsObject:arch]) architecture = YES;
            id deployment = copy[@"macho_min_os"];
            if (!architecture || (deployment && deployment != NSNull.null && (!Version(deployment, &minimum) || minimum > maximum))) continue;
            if (![ids containsObject:copy[@"ipa_id"]] && ids.count < 8) [ids addObject:copy[@"ipa_id"]];
        }
        if (!ids.count) continue;
        VMLegacyStoreVersion *version = existing ?: [self new];
        version->_version = [row[@"version"] copy]; version->_minimumOS = [row[@"minimum_os_version"] copy];
        version->_copyIdentifiers = [ids copy];
        if (!existing) { byVersion[version.version] = version; [versions addObject:version]; }
    }
    [versions sortUsingComparator:^NSComparisonResult(VMLegacyStoreVersion *a, VMLegacyStoreVersion *b) {
        return [b.version compare:a.version options:NSNumericSearch];
    }];
    return versions;
}
@end

@implementation VMLegacyStoreVersions {
    NSURLSessionConfiguration *_configuration;
    VMLegacyStoreRequest *_request;
    NSUInteger _generation;
}
- (instancetype)initWithConfiguration:(NSURLSessionConfiguration *)configuration {
    self = [super init]; if (self) _configuration = [configuration copy]; return self;
}
- (void)cancel { ++_generation; [_request cancel]; _request = nil; }
- (void)loadApp:(VMLegacyStoreApp *)app completion:(void (^)(NSArray<VMLegacyStoreVersion *> *, NSError *))completion {
    [self cancel]; NSUInteger generation = _generation;
    NSURLComponents *URL = [NSURLComponents componentsWithString:@"https://legacystore.app"];
    URL.path = [NSString stringWithFormat:@"/api/v1/apps/%@/versions", app.bundleIdentifier];
    __weak VMLegacyStoreVersions *weakSelf = self;
    _request = [[VMLegacyStoreRequest alloc] initWithURL:URL.URL kind:VMLegacyStoreVersionsRequest
        configuration:_configuration destination:nil expectedSize:0 SHA1:nil progress:nil completion:^(NSData *data, NSError *error) {
            VMLegacyStoreVersions *owner = weakSelf;
            if (!owner || generation != owner->_generation) return;
            owner->_request = nil;
            NSArray *versions = error ? nil : [VMLegacyStoreVersion versionsFromData:data target:app.target error:&error];
            completion(versions, error);
        }];
}
- (void)resolveVersion:(VMLegacyStoreVersion *)version app:(VMLegacyStoreApp *)app
    completion:(void (^)(VMLegacyStoreApp *, NSError *))completion {
    [self cancel];
    [self resolveCopy:0 version:version app:app generation:_generation completion:completion];
}
- (void)resolveCopy:(NSUInteger)index version:(VMLegacyStoreVersion *)version app:(VMLegacyStoreApp *)app
    generation:(NSUInteger)generation completion:(void (^)(VMLegacyStoreApp *, NSError *))completion {
    if (index >= version.copyIdentifiers.count) {
        completion(nil, VMLegacyStoreError(@"No verified copy of this version is available for this guest.")); return;
    }
    NSString *identifier = version.copyIdentifiers[index];
    __weak VMLegacyStoreVersions *weakSelf = self;
    _request = [[VMLegacyStoreRequest alloc] initWithURL:[app.target catalogURLForCopy:identifier] kind:VMLegacyStoreCatalogRequest
        configuration:_configuration destination:nil expectedSize:0 SHA1:nil progress:nil completion:^(NSData *data, NSError *error) {
            VMLegacyStoreVersions *owner = weakSelf;
            if (!owner || owner->_generation != generation) return;
            owner->_request = nil;
            if (error && error.code != 404) { completion(nil, error); return; }
            NSArray *apps = error ? nil : [VMLegacyStoreApp appsFromData:data target:app.target error:&error];
            for (VMLegacyStoreApp *candidate in apps) {
                if ([candidate.identifier isEqual:identifier] && [candidate.bundleIdentifier isEqual:app.bundleIdentifier] &&
                    [candidate.version isEqual:version.version]) { completion(candidate, nil); return; }
            }
            if (error && error.code != 404) { completion(nil, error); return; }
            [owner resolveCopy:index + 1 version:version app:app generation:generation completion:completion];
        }];
}
- (void)dealloc { [_request cancel]; }
@end

@interface VMLegacyStoreRequest () <NSURLSessionDataDelegate>
@property (atomic) BOOL cancelled;
@end
@implementation VMLegacyStoreRequest {
    NSURLSession *_session;
    NSURLSessionDataTask *_task;
    VMLegacyStoreRequestKind _kind;
    NSMutableData *_data;
    NSOutputStream *_stream;
    NSURL *_destination;
    uint64_t _limit, _expected, _received;
    NSString *_SHA1;
    vmfw_sha1_ctx_t _hash;
    NSUInteger _redirects;
    CFAbsoluteTime _lastProgress;
    void (^_progress)(double);
    void (^_completion)(NSData *, NSError *);
}
- (instancetype)initWithURL:(NSURL *)URL kind:(VMLegacyStoreRequestKind)kind
    configuration:(NSURLSessionConfiguration *)configuration destination:(NSURL *)destination
    expectedSize:(uint64_t)size SHA1:(NSString *)SHA1 progress:(void (^)(double))progress
    completion:(void (^)(NSData *, NSError *))completion {
    self = [super init]; if (!self) return nil;
    _kind = kind; _destination = destination; _expected = size; _SHA1 = [SHA1 copy];
    _progress = [progress copy]; _completion = [completion copy];
    _limit = kind == VMLegacyStoreArchiveRequest ? size : kind == VMLegacyStoreIconRequest ? 1024 * 1024 : 2 * 1024 * 1024;
    _data = destination ? nil : [NSMutableData data];
    if (!VMLegacyStoreAllowsURL(URL, kind) || (kind == VMLegacyStoreArchiveRequest &&
        (!destination.isFileURL || !size || size > 512ull * 1024 * 1024 || !Hex(SHA1, 40)))) {
        [self finish:VMLegacyStoreError(@"The download information is invalid.")]; return self;
    }
    if (destination) {
        // Never truncate an existing file, even if a caller accidentally reuses a destination.
        if ([NSFileManager.defaultManager fileExistsAtPath:destination.path]) {
            _destination = nil;
            [self finish:VMLegacyStoreError(@"The temporary download already exists.")]; return self;
        }
        _stream = [NSOutputStream outputStreamWithURL:destination append:NO]; [_stream open];
        if (_stream.streamStatus == NSStreamStatusError || !vmfw_sha1_init(&_hash)) {
            [self finish:VMLegacyStoreError(@"The download could not be saved. Check available storage.")]; return self;
        }
    }
    NSURLSessionConfiguration *config = [configuration copy];
    config.URLCache = nil; config.HTTPCookieStorage = nil; config.URLCredentialStorage = nil;
    config.HTTPShouldSetCookies = NO; config.timeoutIntervalForRequest = 45;
    config.timeoutIntervalForResource = kind == VMLegacyStoreArchiveRequest ? 1800 : 60;
    NSOperationQueue *queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1;
    _session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:queue];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:URL];
    [request setValue:@"S5LBox/1.0 LegacyStore" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
    _task = [_session dataTaskWithRequest:request]; [_task resume];
    return self;
}
- (void)cancel {
    self.cancelled = YES; [_task cancel];
}
- (void)finish:(NSError *)error {
    if (!_completion) return;
    if (self.cancelled) error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil];
    [_stream close]; _stream = nil;
    if (error && _destination) [NSFileManager.defaultManager removeItemAtURL:_destination error:NULL];
    void (^completion)(NSData *, NSError *) = _completion; _completion = nil;
    NSData *data = error ? nil : [_data copy]; _data = nil; _progress = nil;
    [_session invalidateAndCancel]; _session = nil; _task = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.cancelled) {
            if (self->_destination) [NSFileManager.defaultManager removeItemAtURL:self->_destination error:NULL];
            completion(nil, [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]);
        } else completion(data, error);
    });
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completionHandler {
    (void)task; (void)response;
    BOOL allow = session == _session && !self.cancelled && ++_redirects <= 5 && VMLegacyStoreAllowsURL(request.URL, _kind);
    completionHandler(allow ? request : nil);
    if (!allow) [self finish:VMLegacyStoreError(@"The download redirected to an unsupported address.")];
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)task;
    if (session != _session) { completionHandler(NSURLSessionResponseCancel); return; }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
    NSError *error = nil;
    if (!http || !VMLegacyStoreAllowsURL(http.URL, _kind) || http.statusCode != 200) {
        NSString *message = http.statusCode == 429 ? @"Legacy Store is busy. Wait a minute before trying again." :
            http.statusCode == 404 ? @"This archived app is no longer available." : @"The server could not complete the request. Try again later.";
        NSMutableDictionary *details = [@{NSLocalizedDescriptionKey:message} mutableCopy];
        if (http.statusCode == 429 || http.statusCode == 503) {
            NSString *retry = [http valueForHTTPHeaderField:@"Retry-After"];
            NSTimeInterval seconds = Digits(retry, 5) ? MIN(3600, MAX(60, retry.integerValue)) : 60;
            details[@"retryAfter"] = [NSDate dateWithTimeIntervalSinceNow:seconds];
        }
        error = [NSError errorWithDomain:@"S5LBox.LegacyStore" code:http ? http.statusCode : 1 userInfo:details];
    } else if (response.expectedContentLength > (int64_t)_limit ||
        (_expected && response.expectedContentLength >= 0 && (uint64_t)response.expectedContentLength != _expected)) {
        error = VMLegacyStoreError(@"The server returned an unexpected file size.");
    }
    completionHandler(error || self.cancelled ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
    if (error || self.cancelled) [self finish:error];
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)task;
    if (session != _session) return;
    if (self.cancelled) { [self finish:nil]; return; }
    if (data.length > _limit - _received) { [self finish:VMLegacyStoreError(@"The response exceeded its size limit.")]; return; }
    if (_stream) {
        NSUInteger offset = 0;
        while (offset < data.length) {
            NSInteger wrote = [_stream write:(const uint8_t *)data.bytes + offset maxLength:data.length - offset];
            if (wrote <= 0) { [self finish:VMLegacyStoreError(@"The download could not be saved. Check available storage.")]; return; }
            offset += wrote;
        }
        if (!vmfw_sha1_update(&_hash, data.bytes, data.length)) { [self finish:VMLegacyStoreError(@"The checksum could not be calculated.")]; return; }
    } else [_data appendData:data];
    _received += data.length;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (_progress && now - _lastProgress > 0.15) {
        _lastProgress = now;
        double fraction = _expected ? (double)_received / _expected : -1;
        void (^progress)(double) = _progress;
        dispatch_async(dispatch_get_main_queue(), ^{ if (!self.cancelled) progress(fraction); });
    }
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)task;
    if (session != _session) return;
    if (!error && _expected && _received != _expected) error = VMLegacyStoreError(@"The download was incomplete. Try again.");
    if (!error && _SHA1) {
        uint8_t hash[20];
        BOOL valid = vmfw_sha1_final(&_hash, hash);
        NSMutableString *hex = [NSMutableString string];
        if (valid) for (NSUInteger i = 0; i < sizeof hash; i++) [hex appendFormat:@"%02x", hash[i]];
        if (!valid || ![hex isEqual:_SHA1]) error = VMLegacyStoreError(@"The download failed its checksum check. Try again.");
    }
    [self finish:error];
}
@end

@implementation VMLegacyStoreCatalog {
    NSURLSessionConfiguration *_configuration;
    VMLegacyStoreRequest *_request;
    NSMutableDictionary<NSString *, NSDictionary *> *_cache;
    NSUInteger _generation;
    NSDate *_retryAfter;
}
- (instancetype)initWithConfiguration:(NSURLSessionConfiguration *)configuration {
    self = [super init]; if (self) { _configuration = [configuration copy]; _cache = [NSMutableDictionary dictionary]; } return self;
}
- (void)cancel { ++_generation; [_request cancel]; _request = nil; }
- (void)search:(NSString *)query target:(VMLegacyStoreTarget *)target
    completion:(void (^)(NSArray<VMLegacyStoreApp *> *, BOOL, NSError *))completion {
    NSAssert(NSThread.isMainThread, @"Catalog is main-thread owned");
    [self cancel]; NSUInteger generation = _generation;
    NSURL *URL = [target catalogURLForQuery:query];
    NSString *key = URL.absoluteString;
    NSDictionary *cached = _cache[key];
    if (cached && [cached[@"date"] timeIntervalSinceNow] > -300) { completion(cached[@"apps"], NO, nil); return; }
    if (_retryAfter.timeIntervalSinceNow > 0) {
        completion(cached[@"apps"] ?: @[], cached != nil, VMLegacyStoreError(@"Please wait a moment before trying again.")); return;
    }
    __weak VMLegacyStoreCatalog *weakSelf = self;
    _request = [[VMLegacyStoreRequest alloc] initWithURL:URL kind:VMLegacyStoreCatalogRequest configuration:_configuration
        destination:nil expectedSize:0 SHA1:nil progress:nil completion:^(NSData *data, NSError *error) {
            VMLegacyStoreCatalog *owner = weakSelf;
            if (!owner || owner->_generation != generation) return;
            owner->_request = nil;
            NSArray *apps = error ? nil : [VMLegacyStoreApp appsFromData:data target:target error:&error];
            if (apps) {
                if (owner->_cache.count >= 16) [owner->_cache removeAllObjects];
                owner->_cache[key] = @{@"date":NSDate.date, @"apps":apps};
            } else owner->_retryAfter = error.userInfo[@"retryAfter"] ?: [NSDate dateWithTimeIntervalSinceNow:15];
            completion(apps ?: cached[@"apps"] ?: @[], !apps && cached != nil, error);
        }];
}
- (void)dealloc { [_request cancel]; }
@end
