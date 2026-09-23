#import <UIKit/UIKit.h>

@class LBArchive;

NS_ASSUME_NONNULL_BEGIN

@interface EntryListViewController : UITableViewController <UISearchResultsUpdating>

- (instancetype)initWithArchive:(LBArchive*)archive NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithStyle:(UITableViewStyle)style NS_UNAVAILABLE;
- (instancetype)initWithNibName:(nullable NSString*)nibNameOrNil
                         bundle:(nullable NSBundle*)nibBundleOrNil NS_UNAVAILABLE;
- (nullable instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;

@end

NS_ASSUME_NONNULL_END
