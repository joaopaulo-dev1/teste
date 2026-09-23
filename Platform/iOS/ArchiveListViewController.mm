#import "ArchiveListViewController.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "EntryListViewController.h"
#import "LBArchive.h"

static NSString* const kCellId = @"archive";

@implementation ArchiveListViewController {
    NSArray<LBArchive*>* _archives;
    BOOL _loading;
    NSUInteger _scanGeneration;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Arquivos DAT";
    _archives = @[];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                                      target:self
                                                      action:@selector(importTapped)];
    self.refreshControl = [[UIRefreshControl alloc] init];
    [self.refreshControl addTarget:self action:@selector(rescan) forControlEvents:UIControlEventValueChanged];
    // Files dropped in through Finder / Apple Devices file sharing appear while the app is backgrounded.
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(rescan)
                                               name:UIApplicationWillEnterForegroundNotification
                                             object:nil];
    [self rescan];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (NSURL*)documentsURL {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}

- (NSArray<NSURL*>*)datFilesInDocuments {
    NSError* error = nil;
    NSArray<NSURL*>* contents =
        [NSFileManager.defaultManager contentsOfDirectoryAtURL:[self documentsURL]
                                    includingPropertiesForKeys:@[ NSURLIsRegularFileKey ]
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:&error];
    if (contents == nil) {
        NSLog(@"[ArchiveList] cannot list Documents: %@", error);
        return @[];
    }
    NSMutableArray<NSURL*>* dats = [NSMutableArray array];
    for (NSURL* url in contents) {
        NSNumber* regular = nil;
        [url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        if (regular.boolValue && [url.pathExtension.lowercaseString hasPrefix:@"dat"]) {
            [dats addObject:url];
        }
    }
    [dats sortUsingComparator:^NSComparisonResult(NSURL* a, NSURL* b) {
      return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
    return dats;
}

- (void)rescan {
    const NSUInteger generation = ++_scanGeneration;
    _loading = YES;
    [self.tableView reloadData];
    NSArray<NSURL*>* urls = [self datFilesInDocuments];
    __weak ArchiveListViewController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSMutableArray<LBArchive*>* parsed = [NSMutableArray arrayWithCapacity:urls.count];
      for (NSURL* url in urls) {
          [parsed addObject:[LBArchive archiveByReadingURL:url]];
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        ArchiveListViewController* strongSelf = weakSelf;
        if (strongSelf == nil || generation != strongSelf->_scanGeneration) {
            return;
        }
        strongSelf->_archives = parsed;
        strongSelf->_loading = NO;
        [strongSelf.refreshControl endRefreshing];
        [strongSelf.tableView reloadData];
      });
    });
}

- (void)importTapped {
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ] asCopy:YES];
    picker.allowsMultipleSelection = YES;
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSMutableArray<NSString*>* failures = [NSMutableArray array];
    // asCopy:YES hands us private copies, so the user's originals are never opened for writing.
    for (NSURL* source in urls) {
        NSURL* destination = [[self documentsURL] URLByAppendingPathComponent:source.lastPathComponent];
        NSError* error = nil;
        if ([fm fileExistsAtPath:destination.path] && ![fm removeItemAtURL:destination error:&error]) {
            NSLog(@"[ArchiveList] cannot replace %@: %@", destination.lastPathComponent, error);
            [failures addObject:source.lastPathComponent];
            continue;
        }
        if (![fm moveItemAtURL:source toURL:destination error:&error]) {
            NSError* copyError = nil;
            if (![fm copyItemAtURL:source toURL:destination error:&copyError]) {
                NSLog(@"[ArchiveList] import failed %@: move %@ / copy %@", source.lastPathComponent, error, copyError);
                [failures addObject:source.lastPathComponent];
            }
        }
    }
    [self rescan];
    if (failures.count > 0) {
        UIAlertController* alert =
            [UIAlertController alertControllerWithTitle:@"Falha ao importar"
                                                message:[failures componentsJoinedByString:@"\n"]
                                         preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    }
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_archives.count);
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    if (_loading) {
        return @"Lendo índices...";
    }
    if (_archives.count == 0) {
        return @"Nenhum .DAT importado. Toque em + e escolha os .DAT da sua cópia do jogo, ou copie-os para "
               @"este app pelo compartilhamento de arquivos do Apple Devices / iTunes no Windows. "
               @"O app trabalha só sobre cópias e nunca altera os originais.";
    }
    NSUInteger total = 0;
    for (LBArchive* archive in _archives) {
        total += archive.entryCount;
    }
    return [NSString stringWithFormat:@"%lu arquivos, %lu entradas no total. Deslize para remover a cópia importada.",
                                      static_cast<unsigned long>(_archives.count), static_cast<unsigned long>(total)];
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
    if (cell == nil) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kCellId];
        cell.detailTextLabel.numberOfLines = 0;
    }
    LBArchive* archive = _archives[static_cast<NSUInteger>(indexPath.row)];
    cell.textLabel.text = archive.name;
    if (archive.error != nil) {
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ - erro: %@", [LBArchive formatBytes:archive.fileSize], archive.error];
        cell.detailTextLabel.textColor = UIColor.systemRedColor;
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else {
        NSMutableString* detail = [NSMutableString
            stringWithFormat:@"%lu entradas - %lu stored - %lu LZ2K\n%@ no disco, %@ descomprimido",
                             static_cast<unsigned long>(archive.entryCount), static_cast<unsigned long>(archive.storedCount),
                             static_cast<unsigned long>(archive.compressedCount), [LBArchive formatBytes:archive.fileSize],
                             [LBArchive formatBytes:archive.originalBytes]];
        if (archive.unknownCompressionCount > 0) {
            [detail appendFormat:@"\n%lu com compressão desconhecida", static_cast<unsigned long>(archive.unknownCompressionCount)];
        }
        if (archive.outOfBoundsCount > 0) {
            [detail appendFormat:@"\n%lu fora dos limites", static_cast<unsigned long>(archive.outOfBoundsCount)];
        }
        cell.detailTextLabel.text = detail;
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    LBArchive* archive = _archives[static_cast<NSUInteger>(indexPath.row)];
    if (archive.error != nil) {
        return;
    }
    [self.navigationController pushViewController:[[EntryListViewController alloc] initWithArchive:archive] animated:YES];
}

- (void)tableView:(UITableView*)tableView
    commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
     forRowAtIndexPath:(NSIndexPath*)indexPath {
    if (editingStyle != UITableViewCellEditingStyleDelete) {
        return;
    }
    LBArchive* archive = _archives[static_cast<NSUInteger>(indexPath.row)];
    NSError* error = nil;
    if (![NSFileManager.defaultManager removeItemAtURL:archive.url error:&error]) {
        NSLog(@"[ArchiveList] cannot remove %@: %@", archive.name, error);
    }
    [self rescan];
}

@end
