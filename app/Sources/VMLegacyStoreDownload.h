// One app-owned download, independent of navigation. No automatic installation.
#import "VMLegacyStore.h"
NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSNotificationName const VMLegacyStoreDownloadDidChangeNotification;
typedef NS_ENUM(NSInteger, VMLegacyStoreDownloadState) {
    VMLegacyStoreDownloadIdle, VMLegacyStoreDownloading, VMLegacyStoreChecking,
    VMLegacyStoreDownloaded, VMLegacyStoreDownloadFailed, VMLegacyStoreDownloadCancelled
};
@interface VMLegacyStoreDownload : NSObject
// Main-thread state and methods. Publication runs on a worker, after size/hash
// checks. The publisher must validate the IPA and atomically add it to Library.
@property (nonatomic, readonly) VMLegacyStoreDownloadState state;
@property (nonatomic, strong, readonly, nullable) VMLegacyStoreApp *app;
@property (nonatomic, strong, readonly, nullable) NSURL *libraryURL;
@property (nonatomic, strong, readonly, nullable) NSError *error;
@property (nonatomic, readonly) double fraction;
@property (nonatomic, readonly, getter=isBusy) BOOL busy;
- (instancetype)initWithDirectory:(NSURL *)directory configuration:(NSURLSessionConfiguration *)configuration
    publisher:(NSURL * _Nullable (^)(NSURL *file, VMLegacyStoreApp *app, NSError **error))publisher;
- (BOOL)startApp:(VMLegacyStoreApp *)app;
- (void)cancel;
@end
NS_ASSUME_NONNULL_END
