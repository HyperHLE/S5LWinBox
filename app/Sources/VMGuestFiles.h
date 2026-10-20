// Live, bounded AFC file operations. Worker-thread only. MIT licensed.
#import <Foundation/Foundation.h>
@class VMUSBTransport;
@interface VMGuestFile : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *path;
@property (nonatomic, copy) NSString *kind;
@property (nonatomic) uint64_t size;
@end
@interface VMGuestFiles : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier root:(BOOL)root;
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(double fraction);
- (NSArray<VMGuestFile *> *)list:(NSString *)path error:(NSError **)error;
- (NSURL *)download:(VMGuestFile *)file intoDirectory:(NSURL *)directory error:(NSError **)error;
- (BOOL)upload:(NSURL *)source directory:(NSString *)directory error:(NSError **)error;
- (BOOL)createDirectory:(NSString *)path error:(NSError **)error;
- (BOOL)rename:(VMGuestFile *)file name:(NSString *)name error:(NSError **)error;
- (BOOL)remove:(VMGuestFile *)file error:(NSError **)error;
@end
