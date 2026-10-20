// Foreground firmware acquisition. No firmware is bundled with the app.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
@class VMFirmwareImporter;

NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSNotificationName const VMFirmwareDownloadDidChangeNotification;

typedef NS_ENUM(NSInteger, VMFirmwareDownloadPhase) {
    VMFirmwareDownloadIdle, VMFirmwareDownloadResolving, VMFirmwareDownloadReceiving,
    VMFirmwareDownloadVerifying, VMFirmwareDownloadImporting, VMFirmwareDownloadComplete,
    VMFirmwareDownloadFailed, VMFirmwareDownloadCancelled
};

// Metadata is untrusted even when received over HTTPS. This descriptor also
// keeps identity, URL and size policy independently testable without UIKit.
@interface VMFirmwareDownloadMetadata : NSObject
@property (nonatomic, strong, readonly) NSURL *URL;
@property (nonatomic, copy, readonly) NSString *SHA256;
@property (nonatomic, readonly) uint64_t size;
+ (nullable instancetype)metadataFromData:(NSData *)data error:(NSError **)error;
+ (BOOL)allowsURL:(nullable NSURL *)url metadata:(BOOL)metadata;
@end

@interface VMFirmwareDownloader : NSObject
+ (instancetype)sharedDownloader;
@property (atomic, readonly, getter=isRunning) BOOL running;
@property (atomic, readonly) BOOL cancellationRequested;
@property (atomic, readonly) VMFirmwareDownloadPhase phase;
@property (atomic, readonly) double fraction; // -1 when indeterminate
@property (atomic, strong, readonly, nullable) NSError *error;

// Start/cancel on the main thread. Changes are announced on the main thread.
// The shared operation survives navigation and owns its archive until import
// finishes. Manual imports, their keys, and existing firmware are not removed.
- (void)start;
- (void)cancel;

// Injectable storage/transport for Foundation integration tests. Each instance
// must exclusively own its cache directory; production uses sharedDownloader.
- (instancetype)initWithImporter:(VMFirmwareImporter *)importer
                  cacheDirectory:(NSURL *)directory
                   configuration:(NSURLSessionConfiguration *)configuration;
@end
NS_ASSUME_NONNULL_END
