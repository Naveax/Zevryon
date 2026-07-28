#include "native_metal_window_test_window.hpp"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
#include <thread>

namespace zevryon::text::test {
namespace {

std::uint64_t object_id(id object) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
        (__bridge void*)object));
}

class CocoaMetalWindowTestHost final : public MetalWindowTestHost {
public:
    CocoaMetalWindowTestHost(
        std::uint32_t width,
        std::uint32_t height) noexcept {
        @autoreleasepool {
            NSApplication* application = [NSApplication sharedApplication];
            [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
            const NSRect frame = NSMakeRect(
                100.0,
                100.0,
                static_cast<CGFloat>(width),
                static_cast<CGFloat>(height));
            window_ = [[NSWindow alloc]
                initWithContentRect:frame
                          styleMask:(NSWindowStyleMaskTitled |
                                     NSWindowStyleMaskClosable |
                                     NSWindowStyleMaskResizable)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            if (window_ == nil) {
                return;
            }
            view_ = [[NSView alloc] initWithFrame:NSMakeRect(
                0.0,
                0.0,
                static_cast<CGFloat>(width),
                static_cast<CGFloat>(height))];
            if (view_ == nil) {
                window_ = nil;
                return;
            }
            layer_ = [CAMetalLayer layer];
            if (layer_ == nil) {
                view_ = nil;
                window_ = nil;
                return;
            }
            view_.wantsLayer = YES;
            view_.layer = layer_;
            layer_.frame = view_.bounds;
            layer_.contentsScale = window_.backingScaleFactor;
            layer_.drawableSize = CGSizeMake(width, height);
            window_.contentView = view_;
            [window_ orderFrontRegardless];
            valid_ = true;
            pump_events();
        }
    }

    ~CocoaMetalWindowTestHost() override {
        @autoreleasepool {
            [window_ orderOut:nil];
            window_.contentView = nil;
            view_.layer = nil;
            layer_ = nil;
            view_ = nil;
            window_ = nil;
            pump_events();
        }
    }

    NativeWindowSurfaceHandle handle() const noexcept override {
        NativeWindowSurfaceHandle output;
        if (!valid_) {
            return output;
        }
        output.generation = generation_;
        output.display_or_instance = object_id(window_);
        output.window_or_layer = object_id(layer_);
        output.auxiliary = object_id(view_);
        output.system = NativeWindowSystem::CocoaLayer;
        return output;
    }

    bool resize(std::uint32_t width, std::uint32_t height) noexcept override {
        if (!valid_ || width == 0U || height == 0U) {
            return false;
        }
        @autoreleasepool {
            [window_ setContentSize:NSMakeSize(
                static_cast<CGFloat>(width),
                static_cast<CGFloat>(height))];
            view_.frame = NSMakeRect(
                0.0,
                0.0,
                static_cast<CGFloat>(width),
                static_cast<CGFloat>(height));
            layer_.frame = view_.bounds;
            layer_.contentsScale = window_.backingScaleFactor;
            [view_ layoutSubtreeIfNeeded];
            [window_ displayIfNeeded];
            pump_events();
            return true;
        }
    }

    void set_visible(bool visible) noexcept override {
        if (!valid_) {
            return;
        }
        @autoreleasepool {
            if (visible) {
                [window_ orderFrontRegardless];
            } else {
                [window_ orderOut:nil];
            }
            pump_events();
        }
    }

    void pump_events() noexcept override {
        @autoreleasepool {
            NSApplication* application = [NSApplication sharedApplication];
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(30);
            while (std::chrono::steady_clock::now() < deadline) {
                NSEvent* event = [application
                    nextEventMatchingMask:NSEventMaskAny
                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]
                                   inMode:NSDefaultRunLoopMode
                                  dequeue:YES];
                if (event != nil) {
                    [application sendEvent:event];
                }
                [application updateWindows];
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

private:
    NSWindow* window_{nil};
    NSView* view_{nil};
    CAMetalLayer* layer_{nil};
    std::uint64_t generation_{1U};
    bool valid_{false};
};

} // namespace

std::unique_ptr<MetalWindowTestHost>
make_metal_window_test_host(
    std::uint32_t width,
    std::uint32_t height) noexcept {
    try {
        auto output = std::make_unique<CocoaMetalWindowTestHost>(width, height);
        if (output->handle().window_or_layer == 0U) {
            return nullptr;
        }
        return output;
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text::test

#endif // __APPLE__
