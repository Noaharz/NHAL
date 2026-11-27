/*
 * sys.shell – macOS (NSBundle, NSFileManager, NSWorkspace).
 */
#include "nhal/features/sys/shell.h"
#include "common/sys/shell_util.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/stat.h>

static NSString* executable_dir(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char* raw = (char*)malloc(size + 1u);
    if (!raw) return nil;
    NSString* dir = nil;
    if (_NSGetExecutablePath(raw, &size) == 0) {
        char* real = realpath(raw, NULL);
        if (real) {
            dir = [[NSString stringWithUTF8String:real] stringByDeletingLastPathComponent];
            free(real);
        }
    }
    free(raw);
    return dir;
}

static bool in_bundle(void) {
    return [NSBundle.mainBundle.bundlePath.pathExtension isEqualToString:@"app"];
}

static nhal_status program_dir(char* buf, size_t cap) {
    @autoreleasepool {
        return nhal_shell_put(executable_dir().UTF8String, buf, cap);
    }
}

static nhal_status resource_dir(char* buf, size_t cap) {
    @autoreleasepool {
        if (in_bundle() && NSBundle.mainBundle.resourcePath)
            return nhal_shell_put(NSBundle.mainBundle.resourcePath.UTF8String, buf, cap);
        return nhal_shell_put(executable_dir().UTF8String, buf, cap);
    }
}

static nhal_status user_data_dir(const char* app, char* buf, size_t cap) {
    if (buf && cap) buf[0] = '\0';
    if (!nhal_shell_valid_app_name(app))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "app_name leer, zu lang oder mit Schrägstrich");
    @autoreleasepool {
        NSURL* base = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                            inDomain:NSUserDomainMask
                                                   appropriateForURL:nil
                                                              create:YES
                                                               error:nil];
        if (!base) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Application Support nicht gefunden");
        NSURL* dir = [base URLByAppendingPathComponent:[NSString stringWithUTF8String:app] isDirectory:YES];
        NSError* err = nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil
                                                          error:&err])
            return nhal_status_error(NHAL_ERR_IO, "Ordner für Nutzerdaten ließ sich nicht anlegen");
        return nhal_shell_put(dir.fileSystemRepresentation, buf, cap);
    }
}

static nhal_status open_target(const char* target) {
    if (!target || !target[0]) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziel ist leer");
    @autoreleasepool {
        NSURL* url = nil;
        if (nhal_shell_is_url(target)) {
            url = [NSURL URLWithString:[NSString stringWithUTF8String:target]];
            if (!url) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "URL ist ungültig");
        } else {
            struct stat st;
            if (stat(target, &st) != 0) return nhal_status_error(NHAL_ERR_NOT_FOUND, "Datei oder Ordner gibt es nicht");
            url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:target]];
        }
        if (![NSWorkspace.sharedWorkspace openURL:url])
            return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Das System hat kein Programm dafür geöffnet");
    }
    return nhal_status_ok();
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator;
    return nhal_status_ok();
}

static void feature_shutdown(void) {}

static const nhal_sys_shell_v1 table = {
    sizeof(nhal_sys_shell_v1), program_dir, resource_dir, user_data_dir, open_target,
};

const nhal_feature_impl nhal_impl_sys_shell = { 1u, feature_init, feature_shutdown, &table };
