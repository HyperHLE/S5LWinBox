// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMFirmwareDownloader.h"
#import "VMFirmwareImporter.h"
#include "sha256.h"
#include <math.h>

NSNotificationName const VMFirmwareDownloadDidChangeNotification = @"VMFirmwareDownloadDidChangeNotification";
static NSString *const MetadataURL = @"https://api.ipsw.me/v4/ipsw/iPhone1,2/7E18";
static const NSUInteger MetadataLimit = 64 * 1024;
static const uint64_t ArchiveLimit = 512ull * 1024 * 1024;

static NSError *DownloadError(NSString *message) {
    return [NSError errorWithDomain:@"com.j0shua.S5LBox.FirmwareDownload" code:1
        userInfo:@{NSLocalizedDescriptionKey:message}];
}

@implementation VMFirmwareDownloadMetadata
+ (BOOL)allowsURL:(NSURL *)url metadata:(BOOL)metadata {
    if (![url.scheme.lowercaseString isEqual:@"https"] || url.user.length ||
        url.password.length || url.fragment.length || (url.port && url.port.integerValue != 443)) return NO;
    NSString *host = url.host.lowercaseString;
    return metadata ? [host isEqual:@"api.ipsw.me"] :
        [@[@"secure-appldnld.apple.com", @"appldnld.apple.com", @"updates.cdn-apple.com"] containsObject:host ?: @""];
}
+ (instancetype)metadataFromData:(NSData *)data error:(NSError **)error {
    id json = data.length && data.length <= MetadataLimit
        ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![json isKindOfClass:NSDictionary.class] ||
        ![json[@"identifier"] isEqual:@"iPhone1,2"] || ![json[@"version"] isEqual:@"3.1.3"] ||
        ![json[@"buildid"] isEqual:@"7E18"]) {
        if (error) *error = DownloadError(@"Firmware information is unavailable or doesn’t match iPhone OS 3.1.3. Try again or import an IPSW.");
        return nil;
    }
    NSString *hash = [json[@"sha256sum"] isKindOfClass:NSString.class] ? [json[@"sha256sum"] lowercaseString] : nil;
    NSNumber *size = [json[@"filesize"] isKindOfClass:NSNumber.class] ? json[@"filesize"] : nil;
    NSString *source = [json[@"url"] isKindOfClass:NSString.class] ? json[@"url"] : nil;
    NSURL *url = source ? [NSURL URLWithString:source] : nil;
    NSCharacterSet *hex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"];
    double bytes = size.doubleValue;
    if (hash.length != 64 || [hash rangeOfCharacterFromSet:hex.invertedSet].location != NSNotFound ||
        !size || CFGetTypeID((__bridge CFTypeRef)size) == CFBooleanGetTypeID() ||
        !isfinite(bytes) || bytes < 1 || bytes > ArchiveLimit || floor(bytes) != bytes ||
        ![self allowsURL:url metadata:NO]) {
        if (error) *error = DownloadError(@"The firmware download information is invalid. Try again or import an IPSW.");
        return nil;
    }
    VMFirmwareDownloadMetadata *result = [self new];
    result->_URL = url; result->_SHA256 = hash; result->_size = size.unsignedLongLongValue;
    return result;
}
@end

@interface VMFirmwareDownloader () <NSURLSessionDataDelegate>
@property (atomic, readwrite, getter=isRunning) BOOL running;
@property (atomic, readwrite) BOOL cancellationRequested;
@property (atomic, readwrite) VMFirmwareDownloadPhase phase;
@property (atomic, readwrite) double fraction;
@property (atomic, strong, readwrite, nullable) NSError *error;
@property (atomic, strong, nullable) NSURL *archiveURL;
@end

@implementation VMFirmwareDownloader {
    VMFirmwareImporter *_importer;
    NSURL *_cache;
    NSURLSessionConfiguration *_configuration;
    NSOperationQueue *_queue;
    // These fields are confined to the serial delegate queue. Public state is
    // atomic; UIKit is notified only on main. Streaming avoids holding an IPSW
    // in memory or leaving NSURLSession-owned download files behind.
    NSURLSession *_session;
    NSURLSessionDataTask *_task;
    NSMutableData *_metadata;
    VMFirmwareDownloadMetadata *_descriptor;
    NSURL *_attempt;
    NSOutputStream *_output;
    uint64_t _received;
    ios3_sha256_context_t _digest;
    NSTimeInterval _lastProgress;
}

+ (instancetype)sharedDownloader {
    static VMFirmwareDownloader *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL *cache = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
        shared = [[self alloc] initWithImporter:VMFirmwareImporter.sharedImporter
            cacheDirectory:[cache URLByAppendingPathComponent:@"FirmwareDownloads" isDirectory:YES]
            configuration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    });
    return shared;
}
- (instancetype)initWithImporter:(VMFirmwareImporter *)importer cacheDirectory:(NSURL *)directory
                   configuration:(NSURLSessionConfiguration *)configuration {
    self = [super init];
    if (!self) return nil;
    _importer = importer; _cache = directory; _configuration = [configuration copy];
    _configuration.URLCache = nil;
    _configuration.HTTPCookieStorage = nil;
    _configuration.URLCredentialStorage = nil;
    _configuration.HTTPShouldSetCookies = NO;
    _configuration.timeoutIntervalForRequest = 60;
    _configuration.timeoutIntervalForResource = 1800;
    _queue = [NSOperationQueue new];
    _queue.maxConcurrentOperationCount = 1;
    _queue.qualityOfService = NSQualityOfServiceUtility;
    _fraction = -1;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(importChanged:)
        name:VMFirmwareImportDidChangeNotification object:importer];
    // Clear only this service's abandoned attempts after a terminated app.
    [_queue addOperationWithBlock:^{ [self prepareCache:NULL]; }];
    return self;
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)notify {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:VMFirmwareDownloadDidChangeNotification object:self];
    });
}
- (BOOL)prepareCache:(NSError **)error {
    NSFileManager *files = NSFileManager.defaultManager;
    if (!_cache.isFileURL || ![files createDirectoryAtURL:_cache withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    if (![[files attributesOfItemAtPath:_cache.path error:error][NSFileType] isEqual:NSFileTypeDirectory]) {
        if (error) *error = DownloadError(@"The firmware download folder is unavailable.");
        return NO;
    }
    NSArray<NSURL *> *children = [files contentsOfDirectoryAtURL:_cache includingPropertiesForKeys:nil options:0 error:error];
    if (!children) return NO;
    for (NSURL *child in children) {
        NSString *name = child.lastPathComponent;
        // Unknown files and manual IPSWs are never cleanup targets.
        if (![name hasPrefix:@"attempt-"] || name.length != 44 ||
            ![[NSUUID alloc] initWithUUIDString:[name substringFromIndex:8]]) continue;
        if (![files removeItemAtURL:child error:error]) return NO;
    }
    return YES;
}
- (void)start {
    NSAssert(NSThread.isMainThread, @"Start firmware downloads on main");
    if (self.running || _importer.isRunning) return;
    self.running = YES; self.cancellationRequested = NO; self.error = nil;
    self.phase = VMFirmwareDownloadResolving; self.fraction = -1;
    [self notify];
    [_queue addOperationWithBlock:^{
        NSError *error = nil;
        if (self.cancellationRequested) { [self finish:nil cancelled:YES]; return; }
        if (![self prepareCache:&error]) { [self finish:error ?: DownloadError(@"Couldn’t create the firmware download folder.") cancelled:NO]; return; }
        self->_metadata = [NSMutableData data];
        self->_descriptor = nil; self->_received = 0;
        self->_session = [NSURLSession sessionWithConfiguration:self->_configuration delegate:self delegateQueue:self->_queue];
        [self requestURL:[NSURL URLWithString:MetadataURL]];
    }];
}
- (void)requestURL:(NSURL *)url {
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
    [request setValue:@"S5LBox/1" forHTTPHeaderField:@"User-Agent"];
    _task = [_session dataTaskWithRequest:request];
    [_task resume];
}
- (void)cancel {
    NSAssert(NSThread.isMainThread, @"Cancel firmware downloads on main");
    if (!self.running || self.cancellationRequested) return;
    self.cancellationRequested = YES;
    [self notify];
    if (self.phase == VMFirmwareDownloadImporting && [_importer.selectedURL isEqual:self.archiveURL])
        [_importer cancelImport];
    [_queue addOperationWithBlock:^{
        [self->_task cancel];
        if (!self->_task && self.phase != VMFirmwareDownloadImporting && self.running)
            [self finish:nil cancelled:YES];
    }];
}
- (void)finish:(NSError *)error cancelled:(BOOL)cancelled {
    if (!self.running) return;
    [_output close]; _output = nil;
    [_task cancel]; _task = nil;
    [_session invalidateAndCancel]; _session = nil;
    NSError *cleanupError = nil;
    if (_attempt && ![NSFileManager.defaultManager removeItemAtURL:_attempt error:&cleanupError]) {
        error = DownloadError([NSString stringWithFormat:@"%@ Temporary files could not be removed: %@",
            error.localizedDescription ?: @"Firmware preparation finished.", cleanupError.localizedDescription]);
    }
    _attempt = nil; self.archiveURL = nil; _metadata = nil; _descriptor = nil;
    self.error = error;
    self.phase = error ? VMFirmwareDownloadFailed : cancelled ? VMFirmwareDownloadCancelled : VMFirmwareDownloadComplete;
    self.fraction = self.phase == VMFirmwareDownloadComplete ? 1 : -1;
    self.running = NO;
    [self notify];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completionHandler {
    (void)session; (void)response;
    BOOL allowed = task == _task && [VMFirmwareDownloadMetadata allowsURL:request.URL metadata:self.phase == VMFirmwareDownloadResolving];
    completionHandler(allowed ? request : nil);
    if (!allowed && task == _task) [self finish:DownloadError(@"The firmware server redirected to an unsupported address. Try again or import an IPSW.") cancelled:NO];
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    if (task != _task || self.cancellationRequested) { completionHandler(NSURLSessionResponseCancel); return; }
    BOOL metadata = self.phase == VMFirmwareDownloadResolving;
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (id)response : nil;
    int64_t length = response.expectedContentLength;
    BOOL sizeOK = length == NSURLResponseUnknownLength || (metadata
        ? length >= 0 && length <= (int64_t)MetadataLimit
        : length >= 0 && (uint64_t)length == _descriptor.size);
    if (!http || http.statusCode != 200 || !sizeOK || ![VMFirmwareDownloadMetadata allowsURL:response.URL metadata:metadata]) {
        completionHandler(NSURLSessionResponseCancel);
        [self finish:DownloadError(@"The firmware server returned an incomplete or unexpected response. Try again or import an IPSW.") cancelled:NO];
        return;
    }
    completionHandler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)session;
    if (task != _task) return;
    if (self.cancellationRequested) { [self finish:nil cancelled:YES]; return; }
    if (self.phase == VMFirmwareDownloadResolving) {
        if (data.length > MetadataLimit - _metadata.length) {
            [self finish:DownloadError(@"The firmware information is too large. Try again or import an IPSW.") cancelled:NO]; return;
        }
        [_metadata appendData:data]; return;
    }
    if (data.length > _descriptor.size - _received) {
        [self finish:DownloadError(@"The download is larger than expected. Try again or import an IPSW.") cancelled:NO]; return;
    }
    const uint8_t *bytes = data.bytes;
    NSUInteger remaining = data.length;
    while (remaining) {
        if (self.cancellationRequested) { [self finish:nil cancelled:YES]; return; }
        NSInteger written = [_output write:bytes maxLength:remaining];
        if (written <= 0) { [self finish:_output.streamError ?: DownloadError(@"Couldn’t save the firmware. Check free storage and try again.") cancelled:NO]; return; }
        bytes += written; remaining -= (NSUInteger)written;
    }
    if (!ios3_sha256_update(&_digest, data.bytes, data.length)) {
        [self finish:DownloadError(@"Couldn’t verify the firmware download.") cancelled:NO]; return;
    }
    _received += data.length;
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (now - _lastProgress >= 0.1 || _received == _descriptor.size) {
        _lastProgress = now; self.fraction = (double)_received / _descriptor.size; [self notify];
    }
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)session;
    if (task != _task) return;
    _task = nil;
    if (self.cancellationRequested) { [self finish:nil cancelled:YES]; return; }
    if (error) {
        [self finish:DownloadError([NSString stringWithFormat:@"Download interrupted. %@ Try again or import an IPSW.", error.localizedDescription]) cancelled:NO]; return;
    }
    if (self.phase == VMFirmwareDownloadResolving) {
        _descriptor = [VMFirmwareDownloadMetadata metadataFromData:_metadata error:&error];
        _metadata = nil;
        if (!_descriptor) { [self finish:error cancelled:NO]; return; }
        NSNumber *available = nil;
        [_cache getResourceValue:&available forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:NULL];
        if (available && available.unsignedLongLongValue < _descriptor.size + 700ull * 1024 * 1024) {
            [self finish:DownloadError(@"Free up at least 1 GB of storage, then try again.") cancelled:NO]; return;
        }
        _attempt = [_cache URLByAppendingPathComponent:[@"attempt-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
        if (![NSFileManager.defaultManager createDirectoryAtURL:_attempt withIntermediateDirectories:NO attributes:nil error:&error]) {
            _attempt = nil; [self finish:error cancelled:NO]; return;
        }
        self.archiveURL = [_attempt URLByAppendingPathComponent:@"iPhone1,2_3.1.3_7E18_Restore.ipsw"];
        _output = [NSOutputStream outputStreamWithURL:self.archiveURL append:NO];
        [_output open];
        if (_output.streamStatus != NSStreamStatusOpen || !ios3_sha256_init(&_digest)) {
            [self finish:_output.streamError ?: DownloadError(@"Couldn’t create the firmware download.") cancelled:NO]; return;
        }
        self.phase = VMFirmwareDownloadReceiving; self.fraction = 0; _lastProgress = 0;
        [self notify]; [self requestURL:_descriptor.URL]; return;
    }
    [_output close];
    NSError *writeError = _output.streamError;
    _output = nil;
    uint64_t saved = [[NSFileManager.defaultManager attributesOfItemAtPath:self.archiveURL.path error:NULL][NSFileSize] unsignedLongLongValue];
    if (writeError || _received != _descriptor.size || saved != _descriptor.size) {
        [self finish:writeError ?: DownloadError(@"The firmware download is incomplete. Try again.") cancelled:NO]; return;
    }
    self.phase = VMFirmwareDownloadVerifying; self.fraction = -1; [self notify];
    uint8_t digest[IOS3_SHA256_DIGEST_SIZE];
    BOOL valid = ios3_sha256_final(&_digest, digest);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    if (valid) for (NSUInteger i = 0; i < sizeof digest; i++) [hex appendFormat:@"%02x", digest[i]];
    if (!valid || ![hex isEqual:_descriptor.SHA256]) {
        [self finish:DownloadError(@"The download failed its checksum check. Try again or import an IPSW.") cancelled:NO]; return;
    }
    if (self.cancellationRequested) { [self finish:nil cancelled:YES]; return; }
    [_session finishTasksAndInvalidate]; _session = nil;
    self.phase = VMFirmwareDownloadImporting; [self notify];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.cancellationRequested || self->_importer.isRunning) {
            BOOL cancelled = self.cancellationRequested;
            [self->_queue addOperationWithBlock:^{
                [self finish:cancelled ? nil : DownloadError(@"Another firmware import is already running.") cancelled:cancelled];
            }];
            return;
        }
        [self->_importer importIPSWAtURL:self.archiveURL];
    });
}
- (void)importChanged:(NSNotification *)notification {
    (void)notification;
    NSAssert(NSThread.isMainThread, @"Import notifications arrive on main");
    if (!self.running || self.phase != VMFirmwareDownloadImporting || _importer.isRunning ||
        ![_importer.selectedURL isEqual:self.archiveURL]) return;
    vm_fw_report_t report;
    if (![_importer getLastReport:&report]) return;
    BOOL cancelled = report.status == VM_FW_ERR_CANCELLED;
    NSError *error = nil;
    if (!cancelled && !vm_fw_report_is_prepared(&report)) {
        NSString *message = [NSString stringWithUTF8String:report.detail];
        error = DownloadError(message.length ? message : @"Couldn’t prepare the downloaded firmware. Try again or import an IPSW.");
    }
    // A deleted temporary archive must not remain the Settings retry target.
    [_importer forgetSelectedURL:self.archiveURL];
    [_queue addOperationWithBlock:^{ [self finish:error cancelled:cancelled]; }];
}
@end
