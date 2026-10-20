// Native live guest file browser. MIT licensed.
#import <UIKit/UIKit.h>
@class VMUSBTransport;
@interface VMGuestFilesViewController : UITableViewController
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@end
