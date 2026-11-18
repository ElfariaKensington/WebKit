// Injected into SafariForWebKitDevelopment by run-safari (part of libSafariDevShims.dylib).
//
// Safari reaches com.apple.Safari.SandboxBroker for every download (download location,
// download bundle creation, archive extraction, quarantine, ...). The broker only accepts
// clients holding com.apple.private.safari.can-use-sandbox-broker, which
// SafariForWebKitDevelopment lacks, so every connection fails with
// NSXPCSharedListenerErrorDomain Code=5 and every download is cancelled immediately.
//
// SafariForWebKitDevelopment is not app-sandboxed, and the broker's classes
// (SafariSandboxBroker, RestrictedSandboxBroker)
// ship inside Safari.framework itself. So instead of talking to the XPC service, hand
// Safari in-process instances of those same classes, and answer the download-location
// view service's calls with the in-process broker.

#import <AppKit/AppKit.h>
#import <mach/mach.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/log.h>
#import <unistd.h>

static os_log_t shimLog(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.webkit.SafariDevShims", "SandboxBroker"); });
    return log;
}

#pragma mark - Block helpers

struct BlockDescriptor {
    unsigned long reserved;
    unsigned long size;
    // Optional copy/dispose helpers, then the signature.
};

struct BlockLayout {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(void*, ...);
    struct BlockDescriptor* descriptor;
};

enum {
    BlockHasCopyDispose = 1 << 25,
    BlockHasSignature = 1 << 30,
};

static NSMethodSignature* blockSignature(id block)
{
    struct BlockLayout* layout = (__bridge struct BlockLayout*)block;
    if (!(layout->flags & BlockHasSignature))
        return nil;
    uint8_t* cursor = (uint8_t*)layout->descriptor + 2 * sizeof(unsigned long);
    if (layout->flags & BlockHasCopyDispose)
        cursor += 2 * sizeof(void*);
    const char* signature = *(const char**)cursor;
    return signature ? [NSMethodSignature signatureWithObjCTypes:signature] : nil;
}

static bool argumentIsError(NSMethodSignature* signature, NSUInteger index)
{
    return strstr([signature getArgumentTypeAtIndex:index], "NSError") != NULL;
}

// Calls a completion handler shaped (object, error), (error, object) or
// (object, error, extra) with a successful result.
static void callCompletion(id completion, id object, const char* context)
{
    if (!completion)
        return;
    NSMethodSignature* signature = blockSignature(completion);
    NSUInteger arguments = signature ? signature.numberOfArguments - 1 : 2; // Minus the block itself.
    bool errorFirst = signature && arguments >= 1 && argumentIsError(signature, 1);
    id first = errorFirst ? nil : object;
    id second = errorFirst ? object : nil;
    os_log(shimLog(), "%{public}s: completing with local broker (%lu args, errorFirst=%d)", context, (unsigned long)arguments, errorFirst);
    if (arguments >= 3) {
        void (^noop)(void) = ^{ };
        ((void (^)(id, id, id))completion)(first, second, noop);
    } else if (arguments == 2)
        ((void (^)(id, id))completion)(first, second);
    else
        ((void (^)(id))completion)(object);
}

#pragma mark - Local broker instances

static id localSandboxBroker(void)
{
    static id broker;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        audit_token_t token = { };
        mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
        task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count);
        Class brokerClass = objc_getClass("SafariSandboxBroker");
        // +sharedSandboxBrokerWithPID:auditToken: only looks up an existing broker; this one creates it.
        SEL makeShared = sel_registerName("makeSharedSandboxBrokerWithPID:auditToken:");
        SEL initWithPID = sel_registerName("initWithPID:auditToken:");
        if (brokerClass && class_getClassMethod(brokerClass, makeShared))
            broker = ((id (*)(id, SEL, int, audit_token_t))objc_msgSend)(brokerClass, makeShared, getpid(), token);
        if (!broker && brokerClass && class_getInstanceMethod(brokerClass, initWithPID))
            broker = ((id (*)(id, SEL, int, audit_token_t))objc_msgSend)([brokerClass alloc], initWithPID, getpid(), token);
        if (!broker)
            os_log_error(shimLog(), "could not create an in-process SafariSandboxBroker; using the XPC service");
        os_log(shimLog(), "created in-process SafariSandboxBroker %p", (__bridge void*)broker);
    });
    return broker;
}

static id localRestrictedSandboxBroker(void)
{
    static id restricted;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id broker = localSandboxBroker();
        Class restrictedClass = objc_getClass("RestrictedSandboxBroker");
        if (broker && restrictedClass)
            restricted = ((id (*)(id, SEL, id))objc_msgSend)([restrictedClass alloc], sel_registerName("initWithSandboxBroker:"), broker);
        if (!restricted)
            restricted = broker;
    });
    return restricted;
}

#pragma mark - Stand-in for DownloadLocationRemoteViewController

typedef void (^DownloadBundleCompletion)(NSURL* bundleURL, NSData* sandboxExtension, NSString* filename, NSError* error);

static NSURL* downloadFolderURL(id broker)
{
    NSURL* folder = ((id (*)(id, SEL))objc_msgSend)(broker, sel_registerName("downloadLocationURL"));
    return folder ?: [NSFileManager.defaultManager URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
}

static bool brokerAlwaysPrompts(id broker)
{
    return ((BOOL (*)(id, SEL))objc_msgSend)(broker, sel_registerName("alwaysPromptForDownloadFolder"));
}

static void createBundle(id broker, NSString* filename, NSURL* folder, bool allowsOverwrite, DownloadBundleCompletion completion)
{
    ((void (*)(id, SEL, id, id, id, BOOL, id))objc_msgSend)(broker, sel_registerName("createDownloadBundleWithSuggestedFilename:inFolderAtURL:tagNames:allowsOverwrite:completionHandler:"), filename, folder, @[], allowsOverwrite, completion);
}

// Implements the DownloadLocationService protocol, which the real
// DownloadLocationServiceViewController serves from inside the broker over ViewBridge.
// That class is an NSServiceViewController and cannot be instantiated in Safari, so the
// download-bundle work goes straight to the in-process SafariSandboxBroker. Unknown
// selectors are swallowed (and logged) instead of raising "unrecognized selector".
@interface SDSLocalDownloadLocationController : NSObject
@end

@implementation SDSLocalDownloadLocationController

- (void)createDownloadBundleWithSuggestedFilename:(NSString*)suggestedFilename downloadLocationRequestType:(NSInteger)requestType completionHandler:(DownloadBundleCompletion)completion
{
    id broker = localSandboxBroker();
    os_log(shimLog(), "createDownloadBundle: requestType=%ld alwaysPrompt=%d", (long)requestType, brokerAlwaysPrompts(broker));
    if (!brokerAlwaysPrompts(broker) && requestType == 0) {
        createBundle(broker, suggestedFilename, downloadFolderURL(broker), false, completion);
        return;
    }
    // "Ask where to save each download" or "Download Linked File As...": let the user pick.
    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = suggestedFilename ?: @"";
        panel.directoryURL = downloadFolderURL(broker);
        if ([panel runModal] != NSModalResponseOK) {
            completion(nil, nil, nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
            return;
        }
        createBundle(broker, panel.URL.lastPathComponent, panel.URL.URLByDeletingLastPathComponent, true, completion);
    });
}

- (void)chooseCustomDownloadLocationWithCompletionHandler:(void (^)(NSURL*, BOOL, BOOL))completion
{
    id broker = localSandboxBroker();
    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.canCreateDirectories = YES;
        panel.directoryURL = downloadFolderURL(broker);
        if ([panel runModal] != NSModalResponseOK) {
            completion(downloadFolderURL(broker), brokerAlwaysPrompts(broker), NO);
            return;
        }
        ((void (*)(id, SEL, id))objc_msgSend)(broker, sel_registerName("setDownloadLocationURL:"), panel.URL);
        completion(panel.URL, brokerAlwaysPrompts(broker), NO);
    });
}

- (void)reportPermissionDeniedForDownloadLocation:(NSURL*)url completionHandler:(void (^)(void))completion
{
    os_log_error(shimLog(), "permission denied for download location");
    if (completion)
        completion();
}

- (void)saveWebPageUsingDataProvider:(id)dataProvider withSuggestedFilename:(NSString*)filename rawSourceMIMEType:(NSString*)mimeType savePanelMode:(NSInteger)mode completionHandler:(void (^)(NSError*))completion
{
    // File > Save As runs its save panel inside the broker's view service; not supported here.
    os_log_error(shimLog(), "Save As is not supported in SafariForWebKitDevelopment");
    if (completion)
        completion([NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError userInfo:nil]);
}

- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
    NSMethodSignature* signature = [super methodSignatureForSelector:selector];
    if (signature)
        return signature;
    os_log_error(shimLog(), "ignoring -[DownloadLocationRemoteViewController %{public}s]", sel_getName(selector));
    return [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

- (void)forwardInvocation:(NSInvocation*)invocation
{
    // Ignored on purpose; see -methodSignatureForSelector:.
}

@end

#pragma mark - Hooks

// Original implementations, used whenever the in-process broker cannot be created, so a
// failure here degrades to stock behavior instead of leaving completion handlers uncalled.
static IMP originalConnection;
static IMP originalSharedConnection;
static IMP originalEstablish;
static IMP originalEstablishSync;
static IMP originalSetUpRemoteViewController;

static void connectionHook(id self, SEL _cmd, id completion)
{
    id broker = localRestrictedSandboxBroker();
    if (!broker) {
        IMP original = originalSharedConnection;
        if ([self isKindOfClass:objc_getClass("SafariSandboxBrokerConnection")]) {
            const char* name = sel_getName(_cmd);
            original = !strcmp(name, "_establishConnectionWithCompletionHandler:") ? originalEstablish
                : !strcmp(name, "_establishSynchronousConnectionWithCompletionHandler:") ? originalEstablishSync : originalConnection;
        }
        ((void (*)(id, SEL, id))original)(self, _cmd, completion);
        return;
    }
    callCompletion(completion, broker, class_getName(object_getClass(self)));
}

static void setUpRemoteViewControllerHook(id self, SEL _cmd, id block)
{
    if (!localSandboxBroker()) {
        ((void (*)(id, SEL, id))originalSetUpRemoteViewController)(self, _cmd, block);
        return;
    }
    static SDSLocalDownloadLocationController* controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [SDSLocalDownloadLocationController new]; });
    callCompletion(block, controller, "DownloadLocationRemoteViewController");
}

static bool replaceMethod(Class cls, bool isClassMethod, const char* selectorName, IMP replacement, IMP* original)
{
    if (!cls)
        return false;
    SEL selector = sel_registerName(selectorName);
    Method method = isClassMethod ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
    if (!method) {
        os_log_error(shimLog(), "%c[%{public}s %{public}s] not found", isClassMethod ? '+' : '-', class_getName(cls), selectorName);
        return false;
    }
    *original = method_setImplementation(method, replacement);
    return true;
}

bool SDSInstallInProcessSandboxBroker(void)
{
    Class connection = objc_getClass("SafariSandboxBrokerConnection");
    Class remoteViewController = objc_getClass("DownloadLocationRemoteViewController");
    Class broker = objc_getClass("SafariSandboxBroker");
    if (!connection || !remoteViewController || !broker)
        return false; // Safari.framework not loaded yet.

    replaceMethod(connection, false, "_connectionWithCompletionHandler:", (IMP)connectionHook, &originalConnection);
    replaceMethod(connection, false, "_establishConnectionWithCompletionHandler:", (IMP)connectionHook, &originalEstablish);
    replaceMethod(connection, false, "_establishSynchronousConnectionWithCompletionHandler:", (IMP)connectionHook, &originalEstablishSync);
    replaceMethod(objc_getClass("WBSSafariSandboxBrokerConnection"), false, "_connectionWithCompletionHandler:", (IMP)connectionHook, &originalSharedConnection);
    replaceMethod(remoteViewController, true, "setUpDownloadLocationRemoteViewControllerUsingBlock:", (IMP)setUpRemoteViewControllerHook, &originalSetUpRemoteViewController);
    os_log(shimLog(), "routed Safari sandbox broker to in-process instances");
    return true;
}
