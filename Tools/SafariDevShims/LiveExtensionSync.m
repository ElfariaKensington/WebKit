// Injected into SafariForWebKitDevelopment (part of libSafariDevShims.dylib).
//
// Watches source .appex bundles (from synced_bundle_map.plist and /Applications) as well
// as ~/Library/Safari/MagicExtensions via FSEvents. When an extension is rebuilt or
// edited on disk, syncs its updated Contents/Resources into MagicExtensions and
// hot-reloads only the affected extension(s) inside SafariWebExtensionsController
// without restarting Safari.

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/log.h>
#import <sqlite3.h>
#import <sys/stat.h>

static os_log_t extSyncLog(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.webkit.SafariDevShims", "LiveExtensionSync"); });
    return log;
}

static NSString* homePath(NSString* relative)
{
    return [NSHomeDirectory() stringByAppendingPathComponent:relative];
}

static NSString* magicExtensionsDir(void)
{
    return homePath(@"Library/Safari/MagicExtensions");
}

static NSString* syncedBundleMapPath(void)
{
    return homePath(@"Library/Application Support/PatchedSafari/synced_bundle_map.plist");
}

static double fileMTime(NSString* path)
{
    struct stat st;
    if (lstat(path.fileSystemRepresentation, &st) != 0)
        return 0.0;
    return (double)st.st_mtimespec.tv_sec + (double)st.st_mtimespec.tv_nsec * 1e-9;
}

static double recursiveMaxMTime(NSString* rootPath)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:rootPath isDirectory:&isDir])
        return 0.0;

    double maxT = fileMTime(rootPath);
    if (!isDir)
        return maxT;

    NSDirectoryEnumerator<NSString*>* enumerator = [fm enumeratorAtPath:rootPath];
    for (NSString* rel in enumerator) {
        NSString* base = rel.lastPathComponent;
        if ([base isEqualToString:@".DS_Store"] || [base hasPrefix:@".staging-"]) {
            if ([base hasPrefix:@".staging-"])
                [enumerator skipDescendants];
            continue;
        }
        double t = fileMTime([rootPath stringByAppendingPathComponent:rel]);
        if (t > maxT)
            maxT = t;
    }
    return maxT;
}

static BOOL isValidManifestAtDir(NSString* dirPath)
{
    NSString* manifestPath = [dirPath stringByAppendingPathComponent:@"manifest.json"];
    NSData* data = [NSData dataWithContentsOfFile:manifestPath];
    if (!data.length)
        return NO;
    NSError* err = nil;
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    return !err && [parsed isKindOfClass:[NSDictionary class]];
}

static BOOL syncDirectoryContents(NSString* srcDir, NSString* dstDir)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* parent = [dstDir stringByDeletingLastPathComponent];
    NSString* staging = [parent stringByAppendingPathComponent:[NSString stringWithFormat:@".staging-%@-%u", dstDir.lastPathComponent, arc4random()]];
    NSError* err = nil;

    [fm removeItemAtPath:staging error:nil];
    if (![fm copyItemAtPath:srcDir toPath:staging error:&err]) {
        os_log_error(extSyncLog(), "copyItemAtPath %{public}@ -> %{public}@ failed: %{public}@", srcDir, staging, err);
        [fm removeItemAtPath:staging error:nil];
        return NO;
    }

    NSURL* dstURL = [NSURL fileURLWithPath:dstDir];
    NSURL* stagingURL = [NSURL fileURLWithPath:staging];
    if ([fm fileExistsAtPath:dstDir]) {
        if (![fm replaceItemAtURL:dstURL withItemAtURL:stagingURL backupItemName:nil options:NSFileManagerItemReplacementUsingNewMetadataOnly resultingItemURL:nil error:&err]) {
            os_log_error(extSyncLog(), "replaceItemAtURL %{public}@ failed: %{public}@", dstDir, err);
            [fm removeItemAtPath:staging error:nil];
            return NO;
        }
    } else {
        if (![fm moveItemAtURL:stagingURL toURL:dstURL error:&err]) {
            os_log_error(extSyncLog(), "moveItemAtURL %{public}@ failed: %{public}@", dstDir, err);
            [fm removeItemAtPath:staging error:nil];
            return NO;
        }
    }
    return YES;
}

static void touchExtensionsDBModifiedDate(NSString* dirName)
{
    NSString* dbPath = [magicExtensionsDir() stringByAppendingPathComponent:@"Extensions.db"];
    sqlite3* db = NULL;
    if (sqlite3_open_v2(dbPath.fileSystemRepresentation, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 1000);
    sqlite3_stmt* stmt = NULL;
    const char* sql = "UPDATE magic_extensions SET modified_date = ? WHERE directory_name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, [NSDate timeIntervalSinceReferenceDate]);
        sqlite3_bind_text(stmt, 2, dirName.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

static NSString* watchRootForSourcePath(NSString* sourcePath)
{
    // Watch the directory containing the enclosing .app bundle so atomic Xcode rebuilds
    // (which replace Foo.app) still deliver FSEvents.
    NSString* cursor = sourcePath;
    while (cursor.length > 1) {
        if ([[cursor pathExtension] isEqualToString:@"app"])
            return [cursor stringByDeletingLastPathComponent];
        cursor = [cursor stringByDeletingLastPathComponent];
    }
    return [sourcePath stringByDeletingLastPathComponent];
}

static void runSafariLauncherSyncOnly(void)
{
    NSString* launcher = homePath(@"Applications/Safari.app/Contents/MacOS/SafariLauncher");
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:launcher])
        return;
    NSTask* task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:launcher];
    task.arguments = @[ @"--sync-only" ];
    task.standardOutput = [NSPipe pipe];
    task.standardError = [NSPipe pipe];
    NSError* err = nil;
    if ([task launchAndReturnError:&err])
        [task waitUntilExit];
}

static BOOL hasNewUnmappedAppexInApplications(NSDictionary* bundleMap)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray<NSString*>* apps = [fm contentsOfDirectoryAtPath:@"/Applications" error:nil];
    for (NSString* appName in apps) {
        if (![appName.pathExtension isEqualToString:@"app"])
            continue;
        NSString* pluginsDir = [[@"/Applications" stringByAppendingPathComponent:appName] stringByAppendingPathComponent:@"Contents/PlugIns"];
        NSArray<NSString*>* plugins = [fm contentsOfDirectoryAtPath:pluginsDir error:nil];
        for (NSString* p in plugins) {
            if (![p.pathExtension isEqualToString:@"appex"])
                continue;
            NSString* infoPath = [[pluginsDir stringByAppendingPathComponent:p] stringByAppendingPathComponent:@"Contents/Info.plist"];
            NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:infoPath];
            NSDictionary* nsExt = [info[@"NSExtension"] isKindOfClass:[NSDictionary class]] ? info[@"NSExtension"] : nil;
            if (![nsExt[@"NSExtensionPointIdentifier"] isEqualToString:@"com.apple.Safari.web-extension"])
                continue;
            NSString* bid = [info[@"CFBundleIdentifier"] isKindOfClass:[NSString class]] ? info[@"CFBundleIdentifier"] : nil;
            if (bid.length && !bundleMap[bid])
                return YES;
        }
    }
    return NO;
}

static dispatch_queue_t gSyncQueue;
static NSMutableDictionary<NSString*, NSNumber*>* gLastSourceMTime;
static NSMutableDictionary<NSString*, NSNumber*>* gLastMagicMTime;
static FSEventStreamRef gEventStream;
static NSSet<NSString*>* gWatchedPaths;
static uint64_t gDebounceGeneration;

static void reloadChangedExtensionsOnMainQueue(NSSet<NSString*>* changedDirNames, BOOL hasNewExtension)
{
    if (!changedDirNames.count && !hasNewExtension)
        return;

    extern void SDSInvalidateMagicExtensionIconCache(void);
    SDSInvalidateMagicExtensionIconCache();

    dispatch_async(dispatch_get_main_queue(), ^{
        id appCtrl = [NSApp delegate];
        if (!appCtrl || ![appCtrl respondsToSelector:sel_registerName("allWebExtensionsControllers")])
            return;

        if (hasNewExtension) {
            Class dbCls = objc_getClass("WBSMagicExtensionsDatabaseController");
            id db = dbCls && [dbCls respondsToSelector:sel_registerName("shared")]
                ? ((id (*)(id, SEL))objc_msgSend)(dbCls, sel_registerName("shared"))
                : nil;
            if (db && [db respondsToSelector:sel_registerName("_magicExtensionsDidUpdate:")])
                ((void (*)(id, SEL, id))objc_msgSend)(db, sel_registerName("_magicExtensionsDidUpdate:"), nil);
        }

        NSArray* controllers = ((id (*)(id, SEL))objc_msgSend)(appCtrl, sel_registerName("allWebExtensionsControllers"));
        for (id ctrl in controllers) {
            NSArray* extensions = [ctrl respondsToSelector:sel_registerName("extensions")]
                ? ((id (*)(id, SEL))objc_msgSend)(ctrl, sel_registerName("extensions"))
                : nil;
            BOOL missingAny = NO;

            for (NSString* dirName in changedDirNames) {
                NSString* expectedID = [NSString stringWithFormat:@"com.apple.Safari.MagicExtensions.%@", dirName];
                id targetExt = nil;
                for (id ext in extensions) {
                    NSString* eid = [ext respondsToSelector:sel_registerName("identifier")]
                        ? ((id (*)(id, SEL))objc_msgSend)(ext, sel_registerName("identifier"))
                        : nil;
                    if ([eid isEqualToString:expectedID]) {
                        targetExt = ext;
                        break;
                    }
                }
                if (!targetExt) {
                    missingAny = YES;
                    continue;
                }
                BOOL isEnabled = [ctrl respondsToSelector:sel_registerName("extensionIsEnabled:")]
                    && ((BOOL (*)(id, SEL, id))objc_msgSend)(ctrl, sel_registerName("extensionIsEnabled:"), targetExt);
                if (!isEnabled) {
                    os_log(extSyncLog(), "extension %{public}@ modified while disabled; skipping load", dirName);
                    continue;
                }

                @try {
                    id oldData = [ctrl respondsToSelector:sel_registerName("webExtensionForExtension:")]
                        ? ((id (*)(id, SEL, id))objc_msgSend)(ctrl, sel_registerName("webExtensionForExtension:"), targetExt)
                        : nil;

                    if ([ctrl respondsToSelector:sel_registerName("unloadExtensionIfNecessary:")])
                        ((void (*)(id, SEL, id))objc_msgSend)(ctrl, sel_registerName("unloadExtensionIfNecessary:"), targetExt);

                    // Drop the cached SafariWebExtension/WKWebExtension so _loadEnabledExtension:
                    // re-parses manifest.json and scripts fresh from disk.
                    if (oldData) {
                        for (NSString* ivarName in @[ @"_extensionUniqueIdentifierToExtensionDataMap", @"_extensionIdentifierToData" ]) {
                            @try {
                                id dict = [ctrl valueForKey:ivarName];
                                if ([dict isKindOfClass:[NSMutableDictionary class]]) {
                                    NSMutableDictionary* mdict = (NSMutableDictionary*)dict;
                                    for (id key in [mdict allKeys]) {
                                        if (mdict[key] == oldData)
                                            [mdict removeObjectForKey:key];
                                    }
                                }
                            } @catch (NSException*) {
                            }
                        }
                    }

                    if ([ctrl respondsToSelector:sel_registerName("_loadEnabledExtension:")])
                        ((void (*)(id, SEL, id))objc_msgSend)(ctrl, sel_registerName("_loadEnabledExtension:"), targetExt);

                    os_log(extSyncLog(), "live-reloaded extension %{public}@", dirName);
                } @catch (NSException* ex) {
                    os_log_error(extSyncLog(), "failed to live-reload %{public}@: %{public}@", dirName, ex);
                }
            }

            if (missingAny || hasNewExtension) {
                @try {
                    if ([ctrl respondsToSelector:sel_registerName("findExtensions")])
                        ((void (*)(id, SEL))objc_msgSend)(ctrl, sel_registerName("findExtensions"));
                    if ([ctrl respondsToSelector:sel_registerName("loadExtraDiscoveredExtensions")])
                        ((void (*)(id, SEL))objc_msgSend)(ctrl, sel_registerName("loadExtraDiscoveredExtensions"));
                } @catch (NSException* ex) {
                    os_log_error(extSyncLog(), "failed to discover new extensions: %{public}@", ex);
                }
            }
        }
    });
}

static void refreshEventStreamIfNeeded(NSDictionary* bundleMap);
static void scheduleScan(double delaySeconds);

static void performScan(BOOL initialSnapshotOnly)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* mapPath = syncedBundleMapPath();
    NSMutableDictionary* bundleMap = [[NSDictionary dictionaryWithContentsOfFile:mapPath] mutableCopy] ?: [NSMutableDictionary dictionary];

    BOOL hasNewExt = NO;
    if (!initialSnapshotOnly && hasNewUnmappedAppexInApplications(bundleMap)) {
        os_log(extSyncLog(), "detected new Safari web extension in /Applications; running sync-only");
        runSafariLauncherSyncOnly();
        bundleMap = [[NSDictionary dictionaryWithContentsOfFile:mapPath] mutableCopy] ?: [NSMutableDictionary dictionary];
        hasNewExt = YES;
    }

    NSMutableSet<NSString*>* changedDirs = [NSMutableSet set];
    BOOL mapModified = NO;
    BOOL needsRetry = NO;

    // 1. Check all mapped .appex sources
    for (NSString* bundleID in [bundleMap allKeys]) {
        NSDictionary* entry = [bundleMap[bundleID] isKindOfClass:[NSDictionary class]] ? bundleMap[bundleID] : nil;
        NSString* dirName = [entry[@"dirName"] isKindOfClass:[NSString class]] ? entry[@"dirName"] : nil;
        NSString* sourcePath = [entry[@"sourcePath"] isKindOfClass:[NSString class]] ? entry[@"sourcePath"] : nil;
        if (!dirName.length || !sourcePath.length)
            continue;

        NSString* resPath = [sourcePath stringByAppendingPathComponent:@"Contents/Resources"];
        NSString* infoPath = [sourcePath stringByAppendingPathComponent:@"Contents/Info.plist"];
        if (![fm fileExistsAtPath:resPath])
            continue;

        double srcMTime = fmax(recursiveMaxMTime(resPath), fileMTime(infoPath));
        NSNumber* prevNum = gLastSourceMTime[dirName];

        if (initialSnapshotOnly || !prevNum) {
            gLastSourceMTime[dirName] = @(srcMTime);
            // Also check if source is already newer than MagicExtensions dir on startup
            if (!initialSnapshotOnly && srcMTime > [entry[@"syncedMTime"] doubleValue]) {
                // Fall through to sync
            } else {
                continue;
            }
        }

        if (srcMTime > prevNum.doubleValue + 1e-6) {
            if (!isValidManifestAtDir(resPath)) {
                needsRetry = YES;
                continue;
            }
            NSString* dstDir = [magicExtensionsDir() stringByAppendingPathComponent:dirName];
            os_log(extSyncLog(), "source modified for %{public}@ (%.3f -> %.3f); syncing", dirName, prevNum.doubleValue, srcMTime);
            if (syncDirectoryContents(resPath, dstDir)) {
                gLastSourceMTime[dirName] = @(srcMTime);
                gLastMagicMTime[dirName] = @(recursiveMaxMTime(dstDir));
                touchExtensionsDBModifiedDate(dirName);

                NSMutableDictionary* updated = [entry mutableCopy];
                updated[@"syncedMTime"] = @((double)floor(srcMTime));
                bundleMap[bundleID] = updated;
                mapModified = YES;

                [changedDirs addObject:dirName];
            }
        }
    }

    if (mapModified) {
        NSData* plistData = [NSPropertyListSerialization dataWithPropertyList:bundleMap format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
        if (plistData)
            [plistData writeToFile:mapPath atomically:YES];
    }

    // 2. Check direct edits inside ~/Library/Safari/MagicExtensions/<dirName>
    NSString* magicRoot = magicExtensionsDir();
    NSArray<NSString*>* subdirs = [fm contentsOfDirectoryAtPath:magicRoot error:nil];
    for (NSString* name in subdirs) {
        if ([name hasPrefix:@"."])
            continue;
        NSString* full = [magicRoot stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:full isDirectory:&isDir] || !isDir)
            continue;
        if (!isValidManifestAtDir(full)) {
            if (!initialSnapshotOnly)
                needsRetry = YES;
            continue;
        }
        double curMTime = recursiveMaxMTime(full);
        NSNumber* prevNum = gLastMagicMTime[name];
        if (initialSnapshotOnly) {
            gLastMagicMTime[name] = @(curMTime);
        } else if (!prevNum || curMTime > prevNum.doubleValue + 1e-6) {
            gLastMagicMTime[name] = @(curMTime);
            [changedDirs addObject:name];
            if (!prevNum)
                hasNewExt = YES;
        }
    }

    refreshEventStreamIfNeeded(bundleMap);

    if (!initialSnapshotOnly && (changedDirs.count || hasNewExt))
        reloadChangedExtensionsOnMainQueue(changedDirs, hasNewExt);

    if (needsRetry && !initialSnapshotOnly)
        scheduleScan(1.0);
}

static void scheduleScan(double delaySeconds)
{
    uint64_t gen = ++gDebounceGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delaySeconds * NSEC_PER_SEC)), gSyncQueue, ^{
        if (gen != gDebounceGeneration)
            return;
        performScan(NO);
    });
}

static void fsEventsCallback(ConstFSEventStreamRef streamRef, void* info, size_t numEvents, void* eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[])
{
    (void)streamRef; (void)info; (void)numEvents; (void)eventFlags; (void)eventIds;
    NSArray<NSString*>* paths = (__bridge NSArray<NSString*>*)eventPaths;
    BOOL relevant = NO;
    for (NSString* p in paths) {
        NSString* base = p.lastPathComponent;
        if ([base hasPrefix:@"Extensions.db"] || [base isEqualToString:@".DS_Store"] || [base hasPrefix:@".staging-"])
            continue;
        relevant = YES;
        break;
    }
    if (relevant)
        scheduleScan(0.6);
}

static void refreshEventStreamIfNeeded(NSDictionary* bundleMap)
{
    NSMutableSet<NSString*>* roots = [NSMutableSet set];
    NSFileManager* fm = [NSFileManager defaultManager];

    for (NSString* p in @[ magicExtensionsDir(), homePath(@"Library/Application Support/PatchedSafari"), @"/Applications" ]) {
        if ([fm fileExistsAtPath:p])
            [roots addObject:p];
    }
    for (NSDictionary* entry in [bundleMap allValues]) {
        if (![entry isKindOfClass:[NSDictionary class]])
            continue;
        NSString* src = [entry[@"sourcePath"] isKindOfClass:[NSString class]] ? entry[@"sourcePath"] : nil;
        if (!src.length)
            continue;
        NSString* root = watchRootForSourcePath(src);
        if (root.length && [fm fileExistsAtPath:root])
            [roots addObject:root];
    }

    if (gEventStream && [roots isEqualToSet:gWatchedPaths])
        return;

    if (gEventStream) {
        FSEventStreamStop(gEventStream);
        FSEventStreamInvalidate(gEventStream);
        FSEventStreamRelease(gEventStream);
        gEventStream = NULL;
    }

    if (!roots.count)
        return;

    gWatchedPaths = [roots copy];
    NSArray<NSString*>* sortedRoots = [[roots allObjects] sortedArrayUsingSelector:@selector(compare:)];
    FSEventStreamContext ctx = { 0, NULL, NULL, NULL, NULL };
    gEventStream = FSEventStreamCreate(
        kCFAllocatorDefault,
        fsEventsCallback,
        &ctx,
        (__bridge CFArrayRef)sortedRoots,
        kFSEventStreamEventIdSinceNow,
        0.5,
        kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);

    if (gEventStream) {
        FSEventStreamSetDispatchQueue(gEventStream, gSyncQueue);
        FSEventStreamStart(gEventStream);
        os_log(extSyncLog(), "watching %lu extension roots via FSEvents", (unsigned long)sortedRoots.count);
    }
}

void SDSInstallLiveExtensionSync(void)
{
    if (![[[NSProcessInfo processInfo] processName] isEqualToString:@"SafariForWebKitDevelopment"])
        return;

    static dispatch_once_t once;
    dispatch_once(&once, ^{
        gSyncQueue = dispatch_queue_create("com.webkit.SafariDevShims.LiveExtensionSync", DISPATCH_QUEUE_SERIAL);
        gLastSourceMTime = [NSMutableDictionary dictionary];
        gLastMagicMTime = [NSMutableDictionary dictionary];

        void (^startWatcher)(void) = ^{
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), gSyncQueue, ^{
                performScan(YES);
            });
        };

        if (NSApp && [NSApp isRunning]) {
            startWatcher();
        } else {
            [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* note) {
                (void)note;
                startWatcher();
            }];
        }
    });
}
