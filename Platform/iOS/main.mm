#import <UIKit/UIKit.h>

#import "AppDelegate.h"

int main(int argc, char* argv[]) {
    NSString* delegateClass = nil;
    @autoreleasepool {
        delegateClass = NSStringFromClass([AppDelegate class]);
    }
    return UIApplicationMain(argc, argv, nil, delegateClass);
}
