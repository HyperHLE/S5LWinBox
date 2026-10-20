#import <UIKit/UIKit.h>
#import "VMLegacyStore.h"

@interface VMLegacyStoreViewController : UITableViewController
- (instancetype)initWithTarget:(VMLegacyStoreTarget *)target;
@property (nonatomic, copy) void (^selectionHandler)(NSURL *URL);
@end
