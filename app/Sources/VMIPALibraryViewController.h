// S5LBox -- individually installable guest IPA library. MIT licensed.
#import <UIKit/UIKit.h>
@class VMLegacyStoreTarget;

@interface VMIPALibraryViewController : UITableViewController
- (instancetype)init;
@property (nonatomic, copy) void (^selectionHandler)(NSURL *url);
@property (nonatomic, strong) VMLegacyStoreTarget *storeTarget;
@end
