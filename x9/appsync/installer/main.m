// SPDX-License-Identifier: MIT
// X9 Installer: install .ipa files on the phone through MobileInstallation
// (LSApplicationWorkspace installApplication, as x9-appinst does). Signature
// acceptance is AppSync's job in installd; this app only moves files and asks.
// Sources of .ipa files: "Open in" / share from Safari or Files (they arrive in
// Documents/Inbox), the Files app's "On My iPhone > X9 Installer" folder,
// the + button (document picker, imported as a copy), and a pasted http(s)
// link downloaded here, so Safari's download storage is not involved.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#import <dlfcn.h>

@interface NSObject (X9Install)
+ (id)defaultWorkspace;
- (BOOL)installApplication:(NSURL *)url withOptions:(NSDictionary *)options error:(NSError **)error;
@end

static NSString *documents(void) {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

// Install one IPA. MobileInstallation may consume its input, so it gets a copy.
static BOOL installIPA(NSString *ipa, NSString **message) {
    dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    Class cls = objc_getClass("LSApplicationWorkspace");
    id ws = [cls respondsToSelector:@selector(defaultWorkspace)] ? [cls defaultWorkspace] : nil;
    if (![ws respondsToSelector:@selector(installApplication:withOptions:error:)]) {
        *message = @"系统安装接口不可用（LSApplicationWorkspace）";
        return NO;
    }
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSError *error = nil;
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&error]) {
        *message = error.localizedDescription;
        return NO;
    }
    NSString *copy = [dir stringByAppendingPathComponent:@"install.ipa"];
    BOOL ok = NO;
    @try {
        if ([fm copyItemAtPath:ipa toPath:copy error:&error])
            ok = [ws installApplication:[NSURL fileURLWithPath:copy]
                            withOptions:@{@"ApplicationType": @"User"} error:&error];
    } @catch (NSException *e) {
        error = [NSError errorWithDomain:@"X9Installer" code:1 userInfo:@{NSLocalizedDescriptionKey: e.reason ?: e.name}];
    } @finally {
        [fm removeItemAtPath:dir error:NULL];
    }
    NSLog(@"[X9 Installer] install %@ -> %d %@", ipa.lastPathComponent, ok, error);
    *message = ok ? @"安装完成，回到主屏幕即可看到。" : (error.localizedDescription ?: @"安装失败");
    if (!ok && error.userInfo[NSUnderlyingErrorKey])
        *message = [*message stringByAppendingFormat:@"\n\n%@", [error.userInfo[NSUnderlyingErrorKey] localizedDescription]];
    return ok;
}

// A name for a downloaded file: the server's suggestion, else the URL's last
// path component, always ending in .ipa and never escaping Documents.
static NSString *downloadName(NSURLResponse *response, NSURL *url) {
    NSString *name = response.suggestedFilename.lastPathComponent;
    if (!name.length || [name isEqual:@"Unknown"]) name = url.lastPathComponent;
    name = [name stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
    if (!name.length || [name hasPrefix:@"."]) name = @"download.ipa";
    if (![name.pathExtension.lowercaseString isEqual:@"ipa"]) name = [name stringByAppendingPathExtension:@"ipa"];
    return name;
}

@interface X9ListController : UITableViewController <UIDocumentPickerDelegate, NSURLSessionDownloadDelegate>
@property (nonatomic, strong) NSArray<NSString *> *files;
@property (nonatomic, strong) NSURLSession *session;
@property (nonatomic, strong) UIAlertController *progressAlert;
@end

@implementation X9ListController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"X9 安装器";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(pick)];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
        initWithTitle:@"网址下载" style:UIBarButtonItemStylePlain target:self action:@selector(askURL)];
    self.refreshControl = [UIRefreshControl new];
    [self.refreshControl addTarget:self action:@selector(reload) forControlEvents:UIControlEventValueChanged];
    [self reload];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self reload];
}

- (void)reload {
    NSMutableArray *found = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *dir in @[documents(), [documents() stringByAppendingPathComponent:@"Inbox"]])
        for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:NULL])
            if ([name.pathExtension.lowercaseString isEqual:@"ipa"])
                [found addObject:[dir stringByAppendingPathComponent:name]];
    [found sortUsingSelector:@selector(localizedStandardCompare:)];
    self.files = found;
    [self.tableView reloadData];
    [self.refreshControl endRefreshing];
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section {
    return self.files.count;
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)section {
    return self.files.count ? @"点按安装，左滑删除文件。" :
        @"还没有 IPA。\n在 Safari 或「文件」里打开 .ipa，选择「共享 → X9 安装器」；"
        @"或放进「文件 → 我的 iPhone → X9 安装器」；或点右上角 + 选择文件；"
        @"或点左上角「网址下载」粘贴 .ipa 链接。";
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip {
    UITableViewCell *cell = [tv dequeueReusableCellWithIdentifier:@"f"] ?:
        [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"f"];
    NSString *path = self.files[ip.row];
    cell.textLabel.text = path.lastPathComponent;
    unsigned long long size = [[NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL] fileSize];
    cell.detailTextLabel.text = [NSByteCountFormatter stringFromByteCount:(long long)size
                                                               countStyle:NSByteCountFormatterCountStyleFile];
    return cell;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip {
    [tv deselectRowAtIndexPath:ip animated:YES];
    NSString *path = self.files[ip.row];
    UIAlertController *ask = [UIAlertController alertControllerWithTitle:@"安装"
        message:path.lastPathComponent preferredStyle:UIAlertControllerStyleAlert];
    [ask addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [ask addAction:[UIAlertAction actionWithTitle:@"安装" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        [self install:path];
    }]];
    [self presentViewController:ask animated:YES completion:nil];
}

- (void)tableView:(UITableView *)tv commitEditingStyle:(UITableViewCellEditingStyle)style forRowAtIndexPath:(NSIndexPath *)ip {
    if (style != UITableViewCellEditingStyleDelete) return;
    [NSFileManager.defaultManager removeItemAtPath:self.files[ip.row] error:NULL];
    [self reload];
}

- (void)install:(NSString *)path {
    UIAlertController *busy = [UIAlertController alertControllerWithTitle:@"正在安装…"
        message:path.lastPathComponent preferredStyle:UIAlertControllerStyleAlert];
    [self presentViewController:busy animated:YES completion:^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSString *message = nil;
            BOOL ok = installIPA(path, &message);
            dispatch_async(dispatch_get_main_queue(), ^{
                [busy dismissViewControllerAnimated:YES completion:^{
                    UIAlertController *done = [UIAlertController alertControllerWithTitle:ok ? @"成功" : @"失败"
                        message:message preferredStyle:UIAlertControllerStyleAlert];
                    [done addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
                    [self presentViewController:done animated:YES completion:nil];
                }];
            });
        });
    }];
}

#pragma mark - Download from URL (bypasses Safari's download storage)

- (void)askURL {
    UIAlertController *ask = [UIAlertController alertControllerWithTitle:@"从网址下载"
        message:@"粘贴 .ipa 的下载链接（http 或 https）" preferredStyle:UIAlertControllerStyleAlert];
    [ask addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"https://example.com/app.ipa";
        field.keyboardType = UIKeyboardTypeURL;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
        NSURL *clip = UIPasteboard.generalPasteboard.hasURLs ? UIPasteboard.generalPasteboard.URL : nil;
        NSString *text = clip.absoluteString ?: (UIPasteboard.generalPasteboard.hasStrings ? UIPasteboard.generalPasteboard.string : nil);
        if ([text hasPrefix:@"http://"] || [text hasPrefix:@"https://"]) field.text = text;
    }];
    [ask addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [ask addAction:[UIAlertAction actionWithTitle:@"下载" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        NSString *text = [ask.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSURL *url = [NSURL URLWithString:text];
        if (![url.scheme.lowercaseString isEqual:@"http"] && ![url.scheme.lowercaseString isEqual:@"https"]) {
            [self showTitle:@"失败" message:@"不是 http/https 链接"];
            return;
        }
        [self download:url];
    }]];
    [self presentViewController:ask animated:YES completion:nil];
}

- (void)showTitle:(NSString *)title message:(NSString *)message {
    UIAlertController *done = [UIAlertController alertControllerWithTitle:title message:message
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [done addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:done animated:YES completion:nil];
}

- (void)download:(NSURL *)url {
    if (!self.session)
        self.session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                     delegate:self delegateQueue:NSOperationQueue.mainQueue];
    NSURLSessionDownloadTask *task = [self.session downloadTaskWithURL:url];
    self.progressAlert = [UIAlertController alertControllerWithTitle:@"正在下载…"
        message:url.lastPathComponent preferredStyle:UIAlertControllerStyleAlert];
    [self.progressAlert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel
                                                         handler:^(__unused UIAlertAction *a) { [task cancel]; }]];
    [self presentViewController:self.progressAlert animated:YES completion:nil];
    NSLog(@"[X9 Installer] download %@", url);
    [task resume];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
      didWriteData:(int64_t)bytes totalBytesWritten:(int64_t)written totalBytesExpectedToWrite:(int64_t)total {
    NSString *got = [NSByteCountFormatter stringFromByteCount:written countStyle:NSByteCountFormatterCountStyleFile];
    self.progressAlert.message = total > 0 ?
        [NSString stringWithFormat:@"%@ / %@（%d%%）", got,
            [NSByteCountFormatter stringFromByteCount:total countStyle:NSByteCountFormatterCountStyleFile],
            (int)(written * 100 / total)] : got;
}

// The temporary file disappears when this returns, so it is moved here.
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location {
    NSInteger status = [task.response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)task.response).statusCode : 200;
    if (status < 200 || status >= 300) {
        objc_setAssociatedObject(task, "x9err", [NSString stringWithFormat:@"服务器返回 HTTP %ld", (long)status], OBJC_ASSOCIATION_RETAIN);
        return;
    }
    NSString *dest = [documents() stringByAppendingPathComponent:downloadName(task.response, task.originalRequest.URL)];
    NSError *error = nil;
    [NSFileManager.defaultManager removeItemAtPath:dest error:NULL];
    if (![NSFileManager.defaultManager moveItemAtURL:location toURL:[NSURL fileURLWithPath:dest] error:&error])
        objc_setAssociatedObject(task, "x9err", error.localizedDescription ?: @"保存失败", OBJC_ASSOCIATION_RETAIN);
    else
        objc_setAssociatedObject(task, "x9dest", dest, OBJC_ASSOCIATION_RETAIN);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    NSString *dest = objc_getAssociatedObject(task, "x9dest");
    NSString *message = objc_getAssociatedObject(task, "x9err") ?: error.localizedDescription;
    NSLog(@"[X9 Installer] download done %@ error=%@", dest, message);
    UIAlertController *progress = self.progressAlert;
    self.progressAlert = nil;
    [self reload];
    void (^after)(void) = ^{
        if (error.code == NSURLErrorCancelled) return;
        if (!dest) { [self showTitle:@"下载失败" message:message ?: @"未知错误"]; return; }
        UIAlertController *ask = [UIAlertController alertControllerWithTitle:@"下载完成"
            message:dest.lastPathComponent preferredStyle:UIAlertControllerStyleAlert];
        [ask addAction:[UIAlertAction actionWithTitle:@"稍后" style:UIAlertActionStyleCancel handler:nil]];
        [ask addAction:[UIAlertAction actionWithTitle:@"安装" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
            [self install:dest];
        }]];
        [self presentViewController:ask animated:YES completion:nil];
    };
    if (progress.presentingViewController) [progress dismissViewControllerAnimated:YES completion:after];
    else after();
}

- (void)pick {
    NSMutableArray<UTType *> *types = [NSMutableArray arrayWithObjects:UTTypeZIP, UTTypeData, nil];
    UTType *ipa = [UTType typeWithIdentifier:@"com.apple.itunes.ipa"];
    if (ipa) [types insertObject:ipa atIndex:0];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:types asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    for (NSURL *url in urls) {
        NSString *dest = [documents() stringByAppendingPathComponent:url.lastPathComponent];
        [NSFileManager.defaultManager removeItemAtPath:dest error:NULL];
        [NSFileManager.defaultManager copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:NULL];
    }
    [self reload];
}

@end

@interface X9AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) X9ListController *list;
@end

@implementation X9AppDelegate

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    [NSFileManager.defaultManager createDirectoryAtPath:documents() withIntermediateDirectories:YES attributes:nil error:NULL];
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.list = [[X9ListController alloc] initWithStyle:UITableViewStyleInsetGrouped];
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:self.list];
    [self.window makeKeyAndVisible];
    return YES;
}

// "Open in" / share: the system has already copied the file into Documents/Inbox.
- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary *)options {
    if (url.isFileURL && ![url.path hasPrefix:documents()]) {
        NSString *dest = [documents() stringByAppendingPathComponent:url.lastPathComponent];
        [NSFileManager.defaultManager removeItemAtPath:dest error:NULL];
        BOOL scoped = [url startAccessingSecurityScopedResource];
        [NSFileManager.defaultManager copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:NULL];
        if (scoped) [url stopAccessingSecurityScopedResource];
    }
    [self.list reload];
    return YES;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(X9AppDelegate.class));
    }
}
