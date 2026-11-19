// Thin launcher wrapper for ~/Applications/Safari.app/Contents/MacOS/SafariLauncher.
// Injects libSafariDevShims.dylib into SafariForWebKitDevelopment (UIProcess only;
// XPC services ignore plain DYLD_INSERT_LIBRARIES) and execs SafariLauncherCore.

#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* kShimsPath = "/Users/elfaria/.gemini/antigravity/scratch/webkit/WebKitBuild/Release/libSafariDevShims.dylib";

int main(int argc, char* argv[])
{
    int isSyncMode = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--sync-only") || !strcmp(argv[i], "--dry-run-sync")) {
            isSyncMode = 1;
            break;
        }
    }

    if (!isSyncMode && access(kShimsPath, R_OK) == 0) {
        const char* existing = getenv("DYLD_INSERT_LIBRARIES");
        if (!existing || !*existing) {
            setenv("DYLD_INSERT_LIBRARIES", kShimsPath, 1);
        } else if (!strstr(existing, kShimsPath)) {
            size_t len = strlen(existing) + 1 + strlen(kShimsPath) + 1;
            char* combined = (char*)malloc(len);
            if (combined) {
                snprintf(combined, len, "%s:%s", existing, kShimsPath);
                setenv("DYLD_INSERT_LIBRARIES", combined, 1);
                free(combined);
            }
        }
    }

    char exePath[PATH_MAX];
    uint32_t size = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &size) != 0)
        return 1;

    char resolved[PATH_MAX];
    if (!realpath(exePath, resolved))
        strncpy(resolved, exePath, sizeof(resolved) - 1), resolved[sizeof(resolved) - 1] = '\0';

    char dirBuf[PATH_MAX];
    strncpy(dirBuf, resolved, sizeof(dirBuf) - 1);
    dirBuf[sizeof(dirBuf) - 1] = '\0';
    char* dir = dirname(dirBuf);

    char corePath[PATH_MAX];
    snprintf(corePath, sizeof(corePath), "%s/SafariLauncherCore", dir);

    execv(corePath, argv);
    perror("execv SafariLauncherCore");
    return 127;
}
