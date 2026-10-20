// Native catalog contracts. Metadata is untrusted; no app binaries are bundled.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
@interface VMLegacyStoreTarget : NSObject
@property (nonatomic, copy, readonly) NSString *model;
@property (nonatomic, copy, readonly) NSString *OSVersion;
@property (nonatomic, copy, readonly) NSArray<NSString *> *architectures;
@property (nonatomic, copy, readonly) NSString *family;
// The boot profile of this build, never the host phone's model or iOS version.
// A future boot profile supplies its own target instead of changing API code.
+ (instancetype)supportedGuest;
- (nullable instancetype)initWithModel:(NSString *)model OSVersion:(NSString *)version
                        architectures:(NSArray<NSString *> *)architectures family:(NSString *)family;
- (NSURL *)catalogURLForQuery:(NSString *)query;
- (NSURL *)catalogURLForCopy:(NSString *)identifier;
@end

@interface VMLegacyStoreApp : NSObject
@property (nonatomic, copy, readonly) NSString *identifier;
@property (nonatomic, copy, readonly) NSString *bundleIdentifier;
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, copy, readonly) NSString *developer;
@property (nonatomic, copy, readonly) NSString *version;
@property (nonatomic, copy, readonly) NSString *minimumOS;
@property (nonatomic, copy, readonly) NSString *SHA1;
@property (nonatomic, readonly) uint64_t size;
@property (nonatomic, strong, readonly) NSURL *downloadURL;
@property (nonatomic, strong, readonly, nullable) NSURL *iconURL;
@property (nonatomic, strong, readonly) VMLegacyStoreTarget *target;
+ (nullable NSArray<VMLegacyStoreApp *> *)appsFromData:(NSData *)data
    target:(VMLegacyStoreTarget *)target error:(NSError **)error;
@end

typedef NS_ENUM(NSInteger, VMLegacyStoreRequestKind) {
    VMLegacyStoreCatalogRequest, VMLegacyStoreIconRequest, VMLegacyStoreArchiveRequest,
    VMLegacyStoreVersionsRequest
};
FOUNDATION_EXPORT NSError *VMLegacyStoreError(NSString *message);
FOUNDATION_EXPORT BOOL VMLegacyStoreAllowsURL(NSURL * _Nullable URL, VMLegacyStoreRequestKind kind);

// Bounded, cancellable requests. Archives stream directly to an exclusively
// owned file. All completions/progress arrive on main, including cancellation.
@interface VMLegacyStoreRequest : NSObject
- (instancetype)initWithURL:(NSURL *)URL kind:(VMLegacyStoreRequestKind)kind
    configuration:(NSURLSessionConfiguration *)configuration
    destination:(nullable NSURL *)destination expectedSize:(uint64_t)size SHA1:(nullable NSString *)SHA1
    progress:(nullable void (^)(double fraction))progress
    completion:(void (^)(NSData * _Nullable data, NSError * _Nullable error))completion;
- (void)cancel;
@end

@interface VMLegacyStoreCatalog : NSObject
- (instancetype)initWithConfiguration:(NSURLSessionConfiguration *)configuration;
- (void)search:(NSString *)query target:(VMLegacyStoreTarget *)target
    completion:(void (^)(NSArray<VMLegacyStoreApp *> *apps, BOOL cached, NSError * _Nullable error))completion;
- (void)cancel;
@end

@interface VMLegacyStoreVersion : NSObject
@property (nonatomic, copy, readonly) NSString *version;
@property (nonatomic, copy, readonly) NSString *minimumOS;
@property (nonatomic, copy, readonly, getter=archiveIdentifiers) NSArray<NSString *> *copyIdentifiers;
+ (nullable NSArray<VMLegacyStoreVersion *> *)versionsFromData:(NSData *)data
    target:(VMLegacyStoreTarget *)target error:(NSError **)error;
@end

// Enumerate candidates without downloading IPAs. Selecting one rechecks its
// exact copy against the emulator endpoint and the original app identity.
@interface VMLegacyStoreVersions : NSObject
- (instancetype)initWithConfiguration:(NSURLSessionConfiguration *)configuration;
- (void)loadApp:(VMLegacyStoreApp *)app completion:(void (^)(NSArray<VMLegacyStoreVersion *> * _Nullable versions, NSError * _Nullable error))completion;
- (void)resolveVersion:(VMLegacyStoreVersion *)version app:(VMLegacyStoreApp *)app
    completion:(void (^)(VMLegacyStoreApp * _Nullable selection, NSError * _Nullable error))completion;
- (void)cancel;
@end
NS_ASSUME_NONNULL_END
