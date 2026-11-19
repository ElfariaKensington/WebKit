// Injected into SafariForWebKitDevelopment (part of libSafariDevShims.dylib).
//
// -[SafariExtensionCommandMenuController _reloadMenu] runs on every windowDidBecomeMain:
// and calls -[WBSWebExtensionData magicExtensionIconWithSize:] synchronously on the main
// thread for every extension with commands. Without caching, Magic Extensions backed by
// SF Symbols re-render via ISSymbolIcon/CoreUI/CI::MetalContext (~200-400 ms per focus
// switch). Cache rendered icons per WBSWebExtensionData instance, size, and appearance.

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/lock.h>
#import <os/log.h>

static os_log_t iconCacheLog(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        log = os_log_create("com.webkit.SafariDevShims", "MagicExtensionIconCache");
    });
    return log;
}

static os_unfair_lock gCacheLock = OS_UNFAIR_LOCK_INIT;
static NSMapTable<id, NSMutableDictionary<NSString*, NSImage*>*>* gIconCache;
static IMP gOrigMagicExtensionIconWithSize;
static IMP gOrigMagicExtensionsDidUpdate;

void SDSInvalidateMagicExtensionIconCache(void)
{
    os_unfair_lock_lock(&gCacheLock);
    [gIconCache removeAllObjects];
    os_unfair_lock_unlock(&gCacheLock);
}

static id cachedMagicExtensionIconWithSize(id self, SEL _cmd, CGSize size)
{
    NSString* appearance = NSApp ? ([[NSApp effectiveAppearance] name] ?: @"") : @"";
    NSString* key = [NSString stringWithFormat:@"%.1fx%.1f@%@", size.width, size.height, appearance];

    os_unfair_lock_lock(&gCacheLock);
    if (!gIconCache)
        gIconCache = [NSMapTable weakToStrongObjectsMapTable];
    NSMutableDictionary<NSString*, NSImage*>* perExt = [gIconCache objectForKey:self];
    NSImage* cached = perExt[key];
    os_unfair_lock_unlock(&gCacheLock);
    if (cached)
        return cached;

    id (*orig)(id, SEL, CGSize) = (id (*)(id, SEL, CGSize))gOrigMagicExtensionIconWithSize;
    NSImage* rendered = orig ? orig(self, _cmd, size) : nil;
    if (rendered) {
        os_unfair_lock_lock(&gCacheLock);
        perExt = [gIconCache objectForKey:self];
        if (!perExt) {
            perExt = [NSMutableDictionary dictionaryWithCapacity:4];
            [gIconCache setObject:perExt forKey:self];
        }
        perExt[key] = rendered;
        os_unfair_lock_unlock(&gCacheLock);
    }
    return rendered;
}

static void hookedMagicExtensionsDidUpdate(id self, SEL _cmd, id note)
{
    SDSInvalidateMagicExtensionIconCache();
    void (*orig)(id, SEL, id) = (void (*)(id, SEL, id))gOrigMagicExtensionsDidUpdate;
    if (orig)
        orig(self, _cmd, note);
}

bool SDSInstallMagicExtensionIconCache(void)
{
    if (![[[NSProcessInfo processInfo] processName] isEqualToString:@"SafariForWebKitDevelopment"])
        return true;

    static bool installed;
    if (installed)
        return true;

    Class extCls = objc_getClass("WBSWebExtensionData");
    if (!extCls)
        return false;

    Method m = class_getInstanceMethod(extCls, sel_registerName("magicExtensionIconWithSize:"));
    if (m)
        gOrigMagicExtensionIconWithSize = method_setImplementation(m, (IMP)cachedMagicExtensionIconWithSize);

    Class dbCls = objc_getClass("WBSMagicExtensionsDatabaseController");
    if (dbCls) {
        Method um = class_getInstanceMethod(dbCls, sel_registerName("_magicExtensionsDidUpdate:"));
        if (um)
            gOrigMagicExtensionsDidUpdate = method_setImplementation(um, (IMP)hookedMagicExtensionsDidUpdate);
    }

    installed = true;
    os_log(iconCacheLog(), "installed magicExtensionIconWithSize: cache (%s)", m ? "active" : "selector missing");
    return true;
}
