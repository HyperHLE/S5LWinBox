// Bounded AFC operations over a guest-owned service. MIT licensed.
#import <Foundation/Foundation.h>
@class VMUSBService;
@interface VMUSBAFC : NSObject
- (instancetype)initWithService:(VMUSBService *)service;
// Separate capability: the IPA installer retains its staging-only namespace.
- (instancetype)initForFileBrowserWithService:(VMUSBService *)service;
+ (BOOL)validFilename:(NSString *)name;
- (NSArray<NSString *> *)contentsOfDirectory:(NSString *)path error:(NSError **)error;
- (NSDictionary<NSString *, NSString *> *)attributesOfPath:(NSString *)path error:(NSError **)error;
- (uint64_t)openBrowserPath:(NSString *)path writing:(BOOL)writing error:(NSError **)error;
- (NSData *)readHandle:(uint64_t)handle count:(NSUInteger)count error:(NSError **)error;
- (BOOL)createDirectory:(NSString *)path error:(NSError **)error;
- (BOOL)renameBrowserPath:(NSString *)path toPath:(NSString *)destination error:(NSError **)error;
- (BOOL)removeBrowserPath:(NSString *)path error:(NSError **)error;
- (BOOL)makeStagingDirectory:(NSError **)error;
- (uint64_t)openPath:(NSString *)path error:(NSError **)error;
- (BOOL)writeHandle:(uint64_t)handle data:(NSData *)data error:(NSError **)error;
- (BOOL)closeHandle:(uint64_t)handle error:(NSError **)error;
- (BOOL)removePath:(NSString *)path error:(NSError **)error;
@end
