// FISO Quick Look thumbnail extension for macOS: the Finder shows the album's front
// cover as the icon of every .fiso file. Written against the Objective-C runtime and
// loaded frameworks (no Apple SDK needed to build it).
// API: QuickLookThumbnailing/QLThumbnailProvider.h, QLThumbnailReply.h (macOS 10.15+).
//
// The cover is decoded with ImageIO and drawn into the context Quick Look provides
// (+[QLThumbnailReply replyWithContextSize:drawingBlock:]); no intermediate file is
// written, so nothing depends on another process being able to read the extension's
// sandbox container. Messages go to the system log (Console.app, process FISOThumbnail).
// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace fiso;

typedef void *id;
typedef void *SEL;
typedef void *Class;
typedef void (*IMP)();
typedef const void *CFTypeRef;
typedef void *CGContextRef;
typedef void *CGImageRef;
struct CGSize {
    double width, height;
};
struct CGPoint {
    double x, y;
};
struct CGRect {
    CGPoint origin;
    CGSize size;
};

static void *g_msg;
static SEL (*sel_)(const char *);
static Class (*getClass)(const char *);
static void (*NSLog_)(id, ...);
// CoreFoundation / CoreGraphics / ImageIO
static CFTypeRef (*CFDataCreate_)(CFTypeRef, const uint8_t *, long);
static void (*CFRelease_)(CFTypeRef);
static CFTypeRef (*CGImageSourceCreateWithData_)(CFTypeRef, CFTypeRef);
static CGImageRef (*CGImageSourceCreateImageAtIndex_)(CFTypeRef, size_t, CFTypeRef);
static size_t (*CGImageGetWidth_)(CGImageRef);
static size_t (*CGImageGetHeight_)(CGImageRef);
static void (*CGContextDrawImage_)(CGContextRef, CGRect, CGImageRef);
static void (*CGContextSetInterpolationQuality_)(CGContextRef, int);
static void *g_globalBlockIsa;

template <class R, class... A>
static R send(id obj, const char *sel, A... a) {
    return ((R(*)(id, SEL, A...))g_msg)(obj, sel_(sel), a...);
}
static id nsstr(const std::string &s) {
    return send<id>(getClass("NSString"), "stringWithUTF8String:", s.c_str());
}
static void logMsg(const std::string &m) {
    if (NSLog_) NSLog_(nsstr("FISOThumbnail: %s"), m.c_str());
}

// Clang block ABI.
struct CompletionBlock {  // received from Quick Look: void (^)(QLThumbnailReply *, NSError *)
    void *isa;
    int flags, reserved;
    void (*invoke)(CompletionBlock *, id, id);
};
struct BlockDescriptor {
    unsigned long reserved, size;
    const char *signature;
};
struct DrawBlock {  // created here: BOOL (^)(CGContextRef). Global blocks are never copied or freed.
    void *isa;
    int flags, reserved;
    signed char (*invoke)(DrawBlock *, CGContextRef);
    const BlockDescriptor *descriptor;
    CGImageRef image;  // "captured" state
    CGSize size;
};
static const BlockDescriptor kDrawDesc = {0, sizeof(DrawBlock), "c16@?0^{CGContext=}8"};
enum { BLOCK_IS_GLOBAL = 1 << 28, BLOCK_HAS_SIGNATURE = 1 << 30 };

static signed char drawCover(DrawBlock *b, CGContextRef ctx) {
    if (!ctx || !b->image) return 0;
    if (CGContextSetInterpolationQuality_) CGContextSetInterpolationQuality_(ctx, 3 /* kCGInterpolationHigh */);
    CGContextDrawImage_(ctx, CGRect{{0, 0}, b->size}, b->image);
    return 1;
}

static std::vector<uint8_t> frontCover(const std::string &path) {
    auto img = Image::open(path);
    std::vector<uint8_t> best;
    for (size_t ci : img->pict_chunks) {
        Picture p = img->readPicture(ci);
        if (best.empty() || p.type == 3) {
            best = p.data;
            if (p.type == 3) break;
        }
    }
    return best;
}

static void provide(id, SEL, id request, CompletionBlock *handler) {
    id reply = nullptr;
    std::string path;
    try {
        id url = send<id>(request, "fileURL");
        path = send<const char *>(send<id>(url, "path"), "fileSystemRepresentation");
        CGSize maxSize = ((CGSize(*)(id, SEL))g_msg)(request, sel_("maximumSize"));
        auto data = frontCover(path);
        if (data.empty()) {
            logMsg("no artwork in " + path);
        } else {
            CFTypeRef cfdata = CFDataCreate_(nullptr, data.data(), (long)data.size());
            CFTypeRef src = cfdata ? CGImageSourceCreateWithData_(cfdata, nullptr) : nullptr;
            CGImageRef cg = src ? CGImageSourceCreateImageAtIndex_(src, 0, nullptr) : nullptr;
            if (src) CFRelease_(src);
            if (cfdata) CFRelease_(cfdata);
            if (!cg) {
                logMsg("artwork cannot be decoded in " + path);
            } else {
                double w = (double)CGImageGetWidth_(cg), h = (double)CGImageGetHeight_(cg);
                double mw = maxSize.width > 0 ? maxSize.width : 512, mh = maxSize.height > 0 ? maxSize.height : 512;
                double k = std::min(mw / w, mh / h);
                CGSize sz{std::max(1.0, std::floor(w * k)), std::max(1.0, std::floor(h * k))};
                auto *blk = (DrawBlock *)calloc(1, sizeof(DrawBlock));
                blk->isa = g_globalBlockIsa;
                blk->flags = BLOCK_IS_GLOBAL | BLOCK_HAS_SIGNATURE;
                blk->invoke = drawCover;
                blk->descriptor = &kDrawDesc;
                blk->image = cg;  // kept for the lifetime of this short-lived process
                blk->size = sz;
                reply = ((id(*)(id, SEL, CGSize, void *))g_msg)(getClass("QLThumbnailReply"),
                                                                sel_("replyWithContextSize:drawingBlock:"), sz, blk);
            }
        }
    } catch (const std::exception &e) {
        logMsg("cannot read " + path + ": " + e.what());
        reply = nullptr;  // invalid or unreadable image: Finder keeps the generic icon
    } catch (...) {
        reply = nullptr;
    }
    handler->invoke(handler, reply, nullptr);
}

int main(int argc, char **argv) {
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
    void *foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_GLOBAL);
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW | RTLD_GLOBAL);
    void *imageio = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_NOW | RTLD_GLOBAL);
    void *ql = dlopen("/System/Library/Frameworks/QuickLookThumbnailing.framework/QuickLookThumbnailing", RTLD_NOW | RTLD_GLOBAL);
    if (!objc || !cf || !foundation || !cg || !imageio || !ql) return 1;
    g_msg = dlsym(objc, "objc_msgSend");
    sel_ = (SEL(*)(const char *))dlsym(objc, "sel_registerName");
    getClass = (Class(*)(const char *))dlsym(objc, "objc_getClass");
    NSLog_ = (void (*)(id, ...))dlsym(foundation, "NSLog");
    CFDataCreate_ = (decltype(CFDataCreate_))dlsym(cf, "CFDataCreate");
    CFRelease_ = (decltype(CFRelease_))dlsym(cf, "CFRelease");
    CGImageSourceCreateWithData_ = (decltype(CGImageSourceCreateWithData_))dlsym(imageio, "CGImageSourceCreateWithData");
    CGImageSourceCreateImageAtIndex_ =
        (decltype(CGImageSourceCreateImageAtIndex_))dlsym(imageio, "CGImageSourceCreateImageAtIndex");
    CGImageGetWidth_ = (decltype(CGImageGetWidth_))dlsym(cg, "CGImageGetWidth");
    CGImageGetHeight_ = (decltype(CGImageGetHeight_))dlsym(cg, "CGImageGetHeight");
    CGContextDrawImage_ = (decltype(CGContextDrawImage_))dlsym(cg, "CGContextDrawImage");
    CGContextSetInterpolationQuality_ =
        (decltype(CGContextSetInterpolationQuality_))dlsym(cg, "CGContextSetInterpolationQuality");
    g_globalBlockIsa = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
    auto allocPair = (Class(*)(Class, const char *, size_t))dlsym(objc, "objc_allocateClassPair");
    auto registerPair = (void (*)(Class))dlsym(objc, "objc_registerClassPair");
    auto addMethod = (bool (*)(Class, SEL, IMP, const char *))dlsym(objc, "class_addMethod");
    auto extMain = (int (*)(int, char **))dlsym(foundation, "NSExtensionMain");
    Class base = getClass("QLThumbnailProvider");
    if (!g_msg || !sel_ || !allocPair || !base || !extMain || !CFDataCreate_ || !CGImageSourceCreateWithData_ ||
        !CGImageSourceCreateImageAtIndex_ || !CGContextDrawImage_ || !g_globalBlockIsa) {
        logMsg("a system framework symbol is missing");
        return 1;
    }
    Class cls = allocPair(base, "FISOThumbnailProvider", 0);
    addMethod(cls, sel_("provideThumbnailForFileRequest:completionHandler:"), (IMP)provide, "v@:@@?");
    registerPair(cls);
    logMsg("extension started");
    return extMain(argc, argv);
}
