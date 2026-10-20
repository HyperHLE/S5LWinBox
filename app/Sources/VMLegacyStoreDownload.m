#import "VMLegacyStoreDownload.h"

NSNotificationName const VMLegacyStoreDownloadDidChangeNotification = @"VMLegacyStoreDownloadDidChangeNotification";
@implementation VMLegacyStoreDownload {
    NSURL *_directory, *_attempt;
    NSURLSessionConfiguration *_configuration;
    NSURL *(^_publisher)(NSURL *, VMLegacyStoreApp *, NSError **);
    VMLegacyStoreRequest *_request;
    NSUInteger _generation;
    BOOL _cancelled;
}
- (instancetype)initWithDirectory:(NSURL *)directory configuration:(NSURLSessionConfiguration *)configuration
    publisher:(NSURL *(^)(NSURL *, VMLegacyStoreApp *, NSError **))publisher {
    self = [super init]; if (!self) return nil;
    _directory = directory; _configuration = [configuration copy]; _publisher = [publisher copy];
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:directory.path error:NULL];
    if ([attributes[NSFileType] isEqual:NSFileTypeDirectory]) {
        // Only our own abandoned attempt directories. Never user IPAs or other caches.
        for (NSURL *child in [NSFileManager.defaultManager contentsOfDirectoryAtURL:directory
            includingPropertiesForKeys:nil options:0 error:NULL]) {
            NSString *name = child.lastPathComponent;
            if (name.length == 44 && [name hasPrefix:@"attempt-"] &&
                [[NSUUID alloc] initWithUUIDString:[name substringFromIndex:8]])
                [NSFileManager.defaultManager removeItemAtURL:child error:NULL];
        }
    }
    return self;
}
- (BOOL)isBusy { return _state == VMLegacyStoreDownloading || _state == VMLegacyStoreChecking; }
- (void)notify { [NSNotificationCenter.defaultCenter postNotificationName:VMLegacyStoreDownloadDidChangeNotification object:self]; }
- (void)finishWithURL:(NSURL *)URL error:(NSError *)error {
    [_request cancel]; _request = nil;
    if (_attempt) [NSFileManager.defaultManager removeItemAtURL:_attempt error:NULL];
    _attempt = nil; _libraryURL = URL; _error = error;
    _state = URL ? VMLegacyStoreDownloaded : error.code == NSURLErrorCancelled && [error.domain isEqual:NSURLErrorDomain]
        ? VMLegacyStoreDownloadCancelled : VMLegacyStoreDownloadFailed;
    if (URL) _fraction = 1;
    [self notify];
}
- (BOOL)startApp:(VMLegacyStoreApp *)app {
    NSAssert(NSThread.isMainThread, @"Download state is main-thread owned");
    if (self.busy || !app) return NO;
    NSUInteger generation = ++_generation;
    _cancelled = NO;
    _app = app; _error = nil; _libraryURL = nil; _fraction = 0; _state = VMLegacyStoreDownloading;
    NSFileManager *files = NSFileManager.defaultManager;
    NSError *error = nil;
    NSDictionary *attributes = [files attributesOfItemAtPath:_directory.path error:NULL];
    if (!_directory.isFileURL || (attributes && ![attributes[NSFileType] isEqual:NSFileTypeDirectory]) ||
        ![files createDirectoryAtURL:_directory withIntermediateDirectories:YES attributes:nil error:&error]) {
        [self finishWithURL:nil error:error ?: VMLegacyStoreError(@"Download storage is unavailable.")]; return YES;
    }
    uint64_t free = [[files attributesOfFileSystemForPath:_directory.path error:NULL][NSFileSystemFreeSize] unsignedLongLongValue];
    if (free && free < app.size * 2 + 256ull * 1024 * 1024) {
        [self finishWithURL:nil error:VMLegacyStoreError(@"Not enough free space. Free some storage and try again.")]; return YES;
    }
    _attempt = [_directory URLByAppendingPathComponent:[@"attempt-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
    if (![files createDirectoryAtURL:_attempt withIntermediateDirectories:NO attributes:nil error:&error]) {
        _attempt = nil; [self finishWithURL:nil error:error]; return YES;
    }
    // A readable library name, with no metadata-controlled path components.
    NSMutableCharacterSet *allowed = [NSCharacterSet.alphanumericCharacterSet mutableCopy];
    [allowed addCharactersInString:@" -_"];
    NSString *name = [[app.name componentsSeparatedByCharactersInSet:allowed.invertedSet] componentsJoinedByString:@"-"];
    if (name.length > 40) name = [name substringToIndex:40];
    NSURL *file = [_attempt URLByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.ipa", name.length ? name : @"LegacyStore", app.identifier]];
    [self notify];
    __weak VMLegacyStoreDownload *weakSelf = self;
    _request = [[VMLegacyStoreRequest alloc] initWithURL:app.downloadURL kind:VMLegacyStoreArchiveRequest
        configuration:_configuration destination:file expectedSize:app.size SHA1:app.SHA1 progress:^(double fraction) {
            VMLegacyStoreDownload *owner = weakSelf;
            if (!owner || owner->_generation != generation || owner->_state != VMLegacyStoreDownloading) return;
            owner->_fraction = fraction; [owner notify];
        } completion:^(NSData *data, NSError *requestError) {
            (void)data;
            VMLegacyStoreDownload *owner = weakSelf;
            if (!owner || owner->_generation != generation) return;
            owner->_request = nil;
            if (owner->_cancelled) requestError = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil];
            if (requestError) { [owner finishWithURL:nil error:requestError]; return; }
            // Publication is a short noncancellable transaction; a late Cancel
            // must never remove a successfully published IPA from the user's library.
            owner->_state = VMLegacyStoreChecking; owner->_fraction = 1; [owner notify];
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                @autoreleasepool {
                    NSError *publishError = nil;
                    NSURL *saved = owner->_publisher(file, app, &publishError);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [owner finishWithURL:saved error:publishError ?: (saved ? nil : VMLegacyStoreError(@"The app could not be added to the library."))];
                    });
                }
            });
        }];
    return YES;
}
- (void)cancel {
    NSAssert(NSThread.isMainThread, @"Download state is main-thread owned");
    if (_state == VMLegacyStoreDownloading) { _cancelled = YES; [_request cancel]; }
}
- (void)dealloc { [_request cancel]; }
@end
