#import "EntryListViewController.h"

#import "LBArchive.h"

static NSString* const kCellId = @"entry";

@implementation EntryListViewController {
    LBArchive* _archive;
}

- (instancetype)initWithArchive:(LBArchive*)archive {
    self = [super initWithStyle:UITableViewStylePlain];
    if (self != nil) {
        _archive = archive;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = _archive.name;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    [_archive applyFilter:nil];

    UISearchController* search = [[UISearchController alloc] initWithSearchResultsController:nil];
    search.searchResultsUpdater = self;
    search.obscuresBackgroundDuringPresentation = NO;
    search.searchBar.placeholder = @"Filtrar por caminho (ex.: .an4, chars/batman)";
    search.searchBar.autocapitalizationType = UITextAutocapitalizationTypeNone;
    search.searchBar.autocorrectionType = UITextAutocorrectionTypeNo;
    self.navigationItem.searchController = search;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
}

- (void)updateSearchResultsForSearchController:(UISearchController*)searchController {
    [_archive applyFilter:searchController.searchBar.text];
    [self.tableView reloadData];
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_archive.visibleCount);
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
    return [NSString stringWithFormat:@"%lu de %lu entradas", static_cast<unsigned long>(_archive.visibleCount),
                                      static_cast<unsigned long>(_archive.entryCount)];
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
    if (cell == nil) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kCellId];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        cell.textLabel.lineBreakMode = NSLineBreakByTruncatingHead;
        cell.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    }
    const NSUInteger row = static_cast<NSUInteger>(indexPath.row);
    cell.textLabel.text = [_archive pathAtVisibleRow:row];
    cell.detailTextLabel.text = [_archive detailAtVisibleRow:row];
    return cell;
}

@end
