// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBAFC.h"
#import "VMUSBService.h"
#include <string.h>

static void Put64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}
static uint64_t Get64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static NSData *Word(uint64_t value) {
    uint8_t p[8]; Put64(p, value); return [NSData dataWithBytes:p length:8];
}
static NSError *AFCError(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.AFC" code:code userInfo:@{NSLocalizedDescriptionKey:message}];
}
static NSData *Path(NSString *path) {
    // This client can only modify the one private staging namespace.
    if (![path hasPrefix:@"PublicStaging/S5LBox-"] || ![path hasSuffix:@".ipa"] ||
        path.length > 120 || [path rangeOfString:@"/" options:0 range:NSMakeRange(14, path.length - 14)].location != NSNotFound)
        return nil;
    NSData *bytes = [path dataUsingEncoding:NSUTF8StringEncoding];
    if (!bytes || memchr(bytes.bytes, 0, bytes.length)) return nil;
    NSMutableData *result = [bytes mutableCopy]; uint8_t zero = 0;
    [result appendBytes:&zero length:1]; return result;
}
@implementation VMUSBAFC {
    VMUSBService *_service;
    uint64_t _sequence;
    BOOL _browser;
}
- (instancetype)initWithService:(VMUSBService *)service {
    self = [super init]; if (self) _service = service; return self;
}
- (instancetype)initForFileBrowserWithService:(VMUSBService *)service {
    self = [self initWithService:service]; if (self) _browser = YES; return self;
}
+ (BOOL)validFilename:(NSString *)name {
    NSData *bytes = [name dataUsingEncoding:NSUTF8StringEncoding];
    return bytes.length > 0 && bytes.length <= 255 && ![name isEqual:@"."] && ![name isEqual:@".."] &&
        [name rangeOfString:@"/"].location == NSNotFound &&
        [name rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound;
}
- (NSData *)browserPath:(NSString *)path mutable:(BOOL)mutating {
    if (!_browser || ![path hasPrefix:@"/"] || (mutating && [path isEqual:@"/"])) return nil;
    if (![path isEqual:@"/"]) for (NSString *component in [[path substringFromIndex:1] componentsSeparatedByString:@"/"])
        if (![VMUSBAFC validFilename:component]) return nil;
    NSData *bytes = [path dataUsingEncoding:NSUTF8StringEncoding]; if (!bytes || bytes.length > 1024) return nil;
    NSMutableData *result = [bytes mutableCopy]; uint8_t zero = 0; [result appendBytes:&zero length:1]; return result;
}
- (NSData *)operation:(uint64_t)operation arguments:(NSData *)arguments payload:(NSData *)payload
                reply:(uint64_t)expected allowExists:(BOOL)exists error:(NSError **)error {
    if (!arguments || payload.length > 65536) {
        if (error) *error = AFCError(-1, @"Invalid AFC request."); return nil;
    }
    uint8_t header[40] = {'C','F','A','6','L','P','A','A'};
    Put64(header + 8, 40 + arguments.length + payload.length);
    Put64(header + 16, 40 + arguments.length);
    Put64(header + 24, ++_sequence); Put64(header + 32, operation);
    if (![_service writeData:[NSData dataWithBytes:header length:40] error:error] ||
        ![_service writeData:arguments error:error] ||
        (payload.length && ![_service writeData:payload error:error])) return nil;
    NSData *response = [_service readCount:40 error:error];
    if (response.length != 40) return nil;
    const uint8_t *p = response.bytes;
    uint64_t length = Get64(p + 8), head = Get64(p + 16), code = Get64(p + 32);
    if (memcmp(p, "CFA6LPAA", 8) || Get64(p + 24) != _sequence ||
        length < 40 || length > 4u * 1024u * 1024u || head < 40 || head > length) {
        if (error) *error = AFCError(-1, @"The guest returned an invalid AFC packet."); return nil;
    }
    NSData *body = [_service readCount:(NSUInteger)(length - 40) error:error];
    if (!body) return nil;
    if (code == 1 && body.length == 8) {
        uint64_t status = Get64(body.bytes);
        if (expected == 1 && (!status || (exists && status == 16))) return body;
        if (error) *error = AFCError((NSInteger)status,
            [NSString stringWithFormat:@"Guest file transfer failed (AFC %llu).", (unsigned long long)status]);
        return nil;
    }
    if (code != expected || (expected != 2 && body.length != 8)) {
        if (error) *error = AFCError(-1, @"The guest returned an unexpected AFC response."); return nil;
    }
    return body;
}
- (NSArray<NSString *> *)strings:(NSData *)data error:(NSError **)error {
    if (!data) return nil;
    NSMutableArray *result = [NSMutableArray array]; const uint8_t *p = data.bytes; NSUInteger start = 0;
    for (NSUInteger i = 0; i < data.length; i++) if (!p[i]) {
        NSString *value = [[NSString alloc] initWithBytes:p + start length:i - start encoding:NSUTF8StringEncoding];
        if (!value || result.count >= 8192) goto invalid;
        [result addObject:value]; start = i + 1;
    }
    if (start != data.length) goto invalid;
    return result;
invalid:
    if (error) *error = AFCError(-1, @"The guest returned malformed file information."); return nil;
}
- (NSArray<NSString *> *)contentsOfDirectory:(NSString *)path error:(NSError **)error {
    NSArray *names = [self strings:[self operation:3 arguments:[self browserPath:path mutable:NO]
        payload:nil reply:2 allowExists:NO error:error] error:error];
    if (!names) return nil;
    NSMutableOrderedSet *result = [NSMutableOrderedSet orderedSet];
    for (NSString *name in names) {
        if ([name isEqual:@"."] || [name isEqual:@".."] || !name.length) continue;
        if (![VMUSBAFC validFilename:name] || [result containsObject:name] || result.count >= 4096) {
            if (error) *error = AFCError(-1, @"The guest returned an invalid directory listing."); return nil;
        }
        [result addObject:name];
    }
    return result.array;
}
- (NSDictionary<NSString *, NSString *> *)attributesOfPath:(NSString *)path error:(NSError **)error {
    NSArray *items = [self strings:[self operation:10 arguments:[self browserPath:path mutable:NO]
        payload:nil reply:2 allowExists:NO error:error] error:error];
    if (!items) return nil;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    if (items.count % 2) goto invalid;
    for (NSUInteger i = 0; i < items.count; i += 2) {
        if (![items[i] length] || result[items[i]]) goto invalid;
        result[items[i]] = items[i + 1];
    }
    if (![result[@"st_ifmt"] isKindOfClass:NSString.class]) goto invalid;
    return result;
invalid:
    if (error) *error = AFCError(-1, @"The guest returned invalid file attributes."); return nil;
}
- (uint64_t)openBrowserPath:(NSString *)path writing:(BOOL)writing error:(NSError **)error {
    NSData *name = [self browserPath:path mutable:writing];
    if (!name) { if (error) *error = AFCError(-1, @"Invalid guest file path."); return 0; }
    NSMutableData *args = [Word(writing ? 3 : 1) mutableCopy]; [args appendData:name];
    NSData *result = [self operation:13 arguments:args payload:nil reply:14 allowExists:NO error:error];
    uint64_t handle = result ? Get64(result.bytes) : 0;
    if (result && !handle && error) *error = AFCError(-1, @"The guest returned an invalid file handle.");
    return handle;
}
- (NSData *)readHandle:(uint64_t)handle count:(NSUInteger)count error:(NSError **)error {
    NSMutableData *args = nil;
    if (_browser && handle && count && count <= 65536) { args = [Word(handle) mutableCopy]; [args appendData:Word(count)]; }
    NSData *data = [self operation:15 arguments:args payload:nil reply:2 allowExists:NO error:error];
    if (data.length > count) { if (error) *error = AFCError(-1, @"The guest exceeded the requested read size."); return nil; }
    return data;
}
- (BOOL)createDirectory:(NSString *)path error:(NSError **)error {
    return [self operation:9 arguments:[self browserPath:path mutable:YES] payload:nil reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)renameBrowserPath:(NSString *)path toPath:(NSString *)destination error:(NSError **)error {
    NSData *from = [self browserPath:path mutable:YES], *to = [self browserPath:destination mutable:YES];
    NSMutableData *args = nil;
    if (from && to) { args = [from mutableCopy]; [args appendData:to]; }
    return [self operation:24 arguments:args payload:nil reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)removeBrowserPath:(NSString *)path error:(NSError **)error {
    return [self operation:8 arguments:[self browserPath:path mutable:YES] payload:nil reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)makeStagingDirectory:(NSError **)error {
    return [self operation:9 arguments:[NSData dataWithBytes:"PublicStaging" length:14]
                   payload:nil reply:1 allowExists:YES error:error] != nil;
}
- (uint64_t)openPath:(NSString *)path error:(NSError **)error {
    NSData *name = Path(path);
    if (!name) { if (error) *error = AFCError(-1, @"Invalid staging path."); return 0; }
    NSMutableData *arguments = [Word(3) mutableCopy]; // write-only, create/truncate
    [arguments appendData:name];
    NSData *result = [self operation:13 arguments:arguments payload:nil reply:14 allowExists:NO error:error];
    uint64_t handle = result ? Get64(result.bytes) : 0;
    if (result && !handle && error) *error = AFCError(-1, @"The guest returned an invalid file handle.");
    return handle;
}
- (BOOL)writeHandle:(uint64_t)handle data:(NSData *)data error:(NSError **)error {
    return [self operation:16 arguments:handle ? Word(handle) : nil payload:data reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)closeHandle:(uint64_t)handle error:(NSError **)error {
    return [self operation:20 arguments:handle ? Word(handle) : nil payload:nil reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)removePath:(NSString *)path error:(NSError **)error {
    return [self operation:8 arguments:Path(path) payload:nil reply:1 allowExists:NO error:error] != nil;
}
@end
