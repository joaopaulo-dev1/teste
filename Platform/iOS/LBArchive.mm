#import "LBArchive.h"

#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatProbe/DatListing.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

NSString* stringFromBytes(const std::string& bytes) {
    NSString* utf8 = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding];
    if (utf8 != nil) {
        return utf8;
    }
    // Names are not guaranteed to be UTF-8; Latin-1 maps every byte so nothing is dropped.
    return [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSISOLatin1StringEncoding];
}

std::string asciiLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

}  // namespace

@implementation LBArchive {
    gamecore::dat::DatListing _listing;
    std::vector<std::string> _lowerPaths;
    std::vector<std::uint32_t> _visible;
}

+ (instancetype)archiveByReadingURL:(NSURL*)url {
    return [[self alloc] initWithURL:url];
}

- (instancetype)initWithURL:(NSURL*)url {
    self = [super init];
    if (self == nil) {
        return nil;
    }
    _url = url;
    _name = url.lastPathComponent;

    const std::filesystem::path path(url.fileSystemRepresentation);
    gamecore::io::ReadOnlyFile file;
    std::string error;
    if (!gamecore::io::ReadOnlyFile::open(path, file, error)) {
        _error = stringFromBytes(error);
        NSLog(@"[LBArchive] open failed %@: %@", _name, _error);
        return self;
    }
    _fileSize = file.size();
    if (!gamecore::dat::listDat(file.source(), _listing, error)) {
        _error = stringFromBytes(error);
        NSLog(@"[LBArchive] parse failed %@ (%llu bytes): %@", _name, _fileSize, _error);
        _listing = {};
        return self;
    }

    _lowerPaths.reserve(_listing.entries.size());
    for (const auto& entry : _listing.entries) {
        _lowerPaths.push_back(asciiLower(entry.path));
    }
    [self applyFilter:nil];

    NSLog(@"[LBArchive] %@: %zu entries, stored %llu, LZ2K %llu, unknown %llu, unnamed %llu, out_of_bounds %llu",
          _name, _listing.entries.size(), _listing.storedCount, _listing.compressedCount,
          _listing.unknownCompressionCount, _listing.unnamedCount, _listing.outOfBoundsCount);
    return self;
}

- (NSUInteger)entryCount {
    return _listing.entries.size();
}

- (NSUInteger)storedCount {
    return static_cast<NSUInteger>(_listing.storedCount);
}

- (NSUInteger)compressedCount {
    return static_cast<NSUInteger>(_listing.compressedCount);
}

- (NSUInteger)unknownCompressionCount {
    return static_cast<NSUInteger>(_listing.unknownCompressionCount);
}

- (NSUInteger)unnamedCount {
    return static_cast<NSUInteger>(_listing.unnamedCount);
}

- (NSUInteger)outOfBoundsCount {
    return static_cast<NSUInteger>(_listing.outOfBoundsCount);
}

- (uint64_t)originalBytes {
    return _listing.originalBytes;
}

- (NSUInteger)visibleCount {
    return _visible.size();
}

+ (NSString*)formatBytes:(uint64_t)bytes {
    static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return [NSString stringWithFormat:@"%llu B", bytes];
    }
    return [NSString stringWithFormat:@"%.1f %s", value, units[unit]];
}

- (void)applyFilter:(NSString*)query {
    _visible.clear();
    std::string needle;
    if (query.length > 0) {
        needle = asciiLower(std::string(query.UTF8String));
    }
    _visible.reserve(_listing.entries.size());
    for (std::uint32_t i = 0; i < _listing.entries.size(); ++i) {
        if (needle.empty() || _lowerPaths[i].find(needle) != std::string::npos) {
            _visible.push_back(i);
        }
    }
}

- (NSString*)pathAtVisibleRow:(NSUInteger)row {
    if (row >= _visible.size()) {
        return @"";
    }
    return stringFromBytes(_listing.entries[_visible[row]].path);
}

- (NSString*)detailAtVisibleRow:(NSUInteger)row {
    if (row >= _visible.size()) {
        return @"";
    }
    const auto& entry = _listing.entries[_visible[row]];
    NSMutableString* detail = [NSMutableString stringWithFormat:@"#%u  ", entry.id];
    if (entry.compression == 0) {
        [detail appendFormat:@"stored  %@", [LBArchive formatBytes:entry.storedSize]];
    } else {
        [detail appendFormat:@"%s  %@ -> %@", gamecore::dat::compressionName(entry.compression),
                             [LBArchive formatBytes:entry.storedSize], [LBArchive formatBytes:entry.originalSize]];
        if (entry.compression != 2) {
            [detail appendFormat:@" (code %u)", entry.compression];
        }
    }
    [detail appendFormat:@"  @0x%llX", entry.offset];
    if (!entry.inBounds) {
        [detail appendString:@"  FORA DOS LIMITES"];
    }
    return detail;
}

@end
