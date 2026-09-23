#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Read-only view of one imported DAT. Parsing happens in the constructor and is blocking,
// so build instances off the main thread. Filtering and row access belong to the main thread.
@interface LBArchive : NSObject

@property(nonatomic, readonly) NSURL* url;
@property(nonatomic, readonly) NSString* name;
@property(nonatomic, readonly) uint64_t fileSize;
@property(nonatomic, readonly, nullable) NSString* error;
@property(nonatomic, readonly) NSUInteger entryCount;
@property(nonatomic, readonly) NSUInteger storedCount;
@property(nonatomic, readonly) NSUInteger compressedCount;
@property(nonatomic, readonly) NSUInteger unknownCompressionCount;
@property(nonatomic, readonly) NSUInteger unnamedCount;
@property(nonatomic, readonly) NSUInteger outOfBoundsCount;
@property(nonatomic, readonly) uint64_t originalBytes;
@property(nonatomic, readonly) NSUInteger visibleCount;

+ (instancetype)archiveByReadingURL:(NSURL*)url;
+ (NSString*)formatBytes:(uint64_t)bytes;

- (instancetype)init NS_UNAVAILABLE;

- (void)applyFilter:(nullable NSString*)query;
- (NSString*)pathAtVisibleRow:(NSUInteger)row;
- (NSString*)detailAtVisibleRow:(NSUInteger)row;

@end

NS_ASSUME_NONNULL_END
