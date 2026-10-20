// S5LBox -- bounded, read-only artwork inspection. MIT licensed.
#import <Foundation/Foundation.h>

@interface VMIPAIcon : NSObject
/* Call off the main thread with a local library URL. Missing or damaged icons
 * return an empty list; artwork never grants permission to install an app.
 * Candidates are PNG/CgBI data, in preference order, for the native decoder. */
+ (NSArray<NSData *> *)imageDataCandidatesForURL:(NSURL *)url;
@end
