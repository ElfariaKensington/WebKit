// Injected into SafariForWebKitDevelopment by run-safari.
//
// SafariForWebKitDevelopment lacks the entitlement useractivityd requires for the
// BestAppSuggestion XPC interface. useractivityd rejects the connection, and
// UABestAppSuggestionManager's interruption handler immediately calls
// -startListeningForBestAppSuggestions again, so the two processes spin at
// thousands of reconnects per second (useractivityd, coreduetd and Safari each
// burn 50-70% CPU). Handoff *suggestions* are useless for the dev Safari, so the
// listener becomes a no-op. Safari's own Handoff advertising is unaffected.

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <os/log.h>
#import <mach-o/dyld.h>

bool SDSInstallInProcessSandboxBroker(void); // InProcessSandboxBroker.m
void SDSInstallLiveExtensionSync(void); // LiveExtensionSync.m
bool SDSInstallMagicExtensionIconCache(void); // MagicExtensionIconCache.m

static void noopStartListening(id self, SEL _cmd) { }

static bool tryInstall(void)
{
    static bool installed;
    if (installed)
        return true;
    Class cls = objc_getClass("UABestAppSuggestionManager");
    if (!cls)
        return false;
    Method m = class_getInstanceMethod(cls, sel_registerName("startListeningForBestAppSuggestions"));
    if (!m) {
        os_log_error(OS_LOG_DEFAULT, "SafariDevShims: -[UABestAppSuggestionManager startListeningForBestAppSuggestions] not found");
        return installed = true; // Nothing to patch on this OS; stop trying.
    }
    method_setImplementation(m, (IMP)noopStartListening);
    os_log(OS_LOG_DEFAULT, "SafariDevShims: disabled UABestAppSuggestionManager listening");
    return installed = true;
}

static void imageAdded(const struct mach_header* header, intptr_t slide)
{
    (void)header; (void)slide;
    tryInstall();
    static bool brokerInstalled;
    if (!brokerInstalled && !getenv("SAFARIDEVSHIMS_NO_BROKER"))
        brokerInstalled = SDSInstallInProcessSandboxBroker();
    static bool iconCacheInstalled;
    if (!iconCacheInstalled)
        iconCacheInstalled = SDSInstallMagicExtensionIconCache();
}

__attribute__((constructor))
static void installNoBestAppSuggestionShim(void)
{
    // Covers UserActivity.framework being loaded now or later (the callback also fires for
    // every image already loaded).
    _dyld_register_func_for_add_image(imageAdded);
    // An image's add callback can run before the ObjC runtime registers its classes;
    // retry once the main queue runs.
    dispatch_async(dispatch_get_main_queue(), ^{
        imageAdded(NULL, 0);
        SDSInstallLiveExtensionSync();
    });
}
