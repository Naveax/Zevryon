#include "native_vulkan_wsi_test_window.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <wayland-client.h>
#include <xcb/xcb.h>
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
#include "xdg-shell-client-protocol.h"
#endif
#endif

namespace zevryon::text::test {
namespace {

void set_error(std::string* error, const char* message) noexcept {
    if (error == nullptr) {
        return;
    }
    try {
        *error = message;
    } catch (...) {
        error->clear();
    }
}

} // namespace

struct NativeVulkanTestWindow::State final {
    NativeWindowSystem system{NativeWindowSystem::Headless};
    NativeWindowSurfaceHandle handle;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
#if defined(_WIN32)
    HINSTANCE instance{nullptr};
    HWND window{nullptr};
#elif defined(__linux__)
    xcb_connection_t* xcb_connection{nullptr};
    xcb_window_t xcb_window{0U};
    xcb_screen_t* xcb_screen{nullptr};
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    wl_display* wayland_display{nullptr};
    wl_registry* wayland_registry{nullptr};
    wl_compositor* wayland_compositor{nullptr};
    wl_surface* wayland_surface{nullptr};
    xdg_wm_base* wm_base{nullptr};
    xdg_surface* xdg_surface_handle{nullptr};
    xdg_toplevel* xdg_toplevel_handle{nullptr};
    bool wayland_configured{false};
    wl_registry_listener registry_listener{};
    xdg_wm_base_listener wm_listener{};
    xdg_surface_listener surface_listener{};
    xdg_toplevel_listener toplevel_listener{};
#endif
#endif
};

NativeVulkanTestWindow::NativeVulkanTestWindow() noexcept = default;
NativeVulkanTestWindow::~NativeVulkanTestWindow() { destroy(); }

#if defined(_WIN32)
namespace {
LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}
}
#elif defined(__linux__) && defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
namespace {
void registry_global(
    void* data,
    wl_registry* registry,
    std::uint32_t name,
    const char* interface_name,
    std::uint32_t version) {
    auto* state = static_cast<NativeVulkanTestWindow::State*>(data);
    if (std::strcmp(interface_name, wl_compositor_interface.name) == 0) {
        state->wayland_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(
                registry,
                name,
                &wl_compositor_interface,
                std::min<std::uint32_t>(version, 4U)));
    } else if (std::strcmp(interface_name, xdg_wm_base_interface.name) == 0) {
        state->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(
                registry,
                name,
                &xdg_wm_base_interface,
                std::min<std::uint32_t>(version, 6U)));
    }
}
void registry_remove(void*, wl_registry*, std::uint32_t) {}
void wm_ping(void*, xdg_wm_base* base, std::uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
void surface_configure(void* data, xdg_surface* surface, std::uint32_t serial) {
    auto* state = static_cast<NativeVulkanTestWindow::State*>(data);
    xdg_surface_ack_configure(surface, serial);
    state->wayland_configured = true;
}
void toplevel_configure(
    void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
void toplevel_configure_bounds(
    void*, xdg_toplevel*, std::int32_t, std::int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
}
#endif

bool NativeVulkanTestWindow::create(
    NativeWindowSystem system,
    std::uint32_t width,
    std::uint32_t height,
    std::string* error) noexcept {
    destroy();
    if (width == 0U || height == 0U) {
        set_error(error, "test window extent is empty");
        return false;
    }
    try {
        state_ = std::make_unique<State>();
    } catch (...) {
        set_error(error, "test window state allocation failed");
        return false;
    }
    state_->system = system;
    state_->width = width;
    state_->height = height;
#if defined(_WIN32)
    if (system != NativeWindowSystem::Win32) {
        set_error(error, "requested test window system is unavailable");
        destroy();
        return false;
    }
    state_->instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"ZevryonVulkanWsiTestWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = state_->instance;
    window_class.lpszClassName = class_name;
    (void)RegisterClassW(&window_class);
    state_->window = CreateWindowExW(
        0U,
        class_name,
        L"Zevryon Vulkan WSI",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(width),
        static_cast<int>(height),
        nullptr,
        nullptr,
        state_->instance,
        nullptr);
    if (state_->window == nullptr) {
        set_error(error, "CreateWindowExW failed");
        destroy();
        return false;
    }
    ShowWindow(state_->window, SW_SHOWNA);
    state_->handle.generation = 1U;
    state_->handle.display_or_instance = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(state_->instance));
    state_->handle.window_or_layer = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(state_->window));
    state_->handle.system = system;
    return true;
#elif defined(__linux__)
    if (system == NativeWindowSystem::Xcb) {
        int screen_index = 0;
        state_->xcb_connection = xcb_connect(nullptr, &screen_index);
        if (state_->xcb_connection == nullptr ||
            xcb_connection_has_error(state_->xcb_connection) != 0) {
            set_error(error, "xcb_connect failed");
            destroy();
            return false;
        }
        const xcb_setup_t* setup = xcb_get_setup(state_->xcb_connection);
        xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
        for (int index = 0; index < screen_index && iterator.rem != 0;
             ++index) {
            xcb_screen_next(&iterator);
        }
        state_->xcb_screen = iterator.data;
        if (state_->xcb_screen == nullptr) {
            set_error(error, "XCB screen discovery failed");
            destroy();
            return false;
        }
        state_->xcb_window = xcb_generate_id(state_->xcb_connection);
        const std::uint32_t event_mask =
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_create_window(
            state_->xcb_connection,
            XCB_COPY_FROM_PARENT,
            state_->xcb_window,
            state_->xcb_screen->root,
            0,
            0,
            static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height),
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            state_->xcb_screen->root_visual,
            XCB_CW_EVENT_MASK,
            &event_mask);
        xcb_map_window(state_->xcb_connection, state_->xcb_window);
        xcb_flush(state_->xcb_connection);
        state_->handle.generation = 1U;
        state_->handle.display_or_instance = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(state_->xcb_connection));
        state_->handle.window_or_layer = state_->xcb_window;
        state_->handle.system = system;
        return true;
    }
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    if (system == NativeWindowSystem::Wayland) {
        state_->wayland_display = wl_display_connect(nullptr);
        if (state_->wayland_display == nullptr) {
            set_error(error, "wl_display_connect failed");
            destroy();
            return false;
        }
        state_->wayland_registry =
            wl_display_get_registry(state_->wayland_display);
        state_->registry_listener.global = registry_global;
        state_->registry_listener.global_remove = registry_remove;
        wl_registry_add_listener(
            state_->wayland_registry,
            &state_->registry_listener,
            state_.get());
        if (wl_display_roundtrip(state_->wayland_display) < 0 ||
            state_->wayland_compositor == nullptr ||
            state_->wm_base == nullptr) {
            set_error(error, "Wayland compositor or xdg_wm_base is unavailable");
            destroy();
            return false;
        }
        state_->wm_listener.ping = wm_ping;
        xdg_wm_base_add_listener(
            state_->wm_base, &state_->wm_listener, state_.get());
        state_->wayland_surface =
            wl_compositor_create_surface(state_->wayland_compositor);
        state_->xdg_surface_handle =
            xdg_wm_base_get_xdg_surface(
                state_->wm_base, state_->wayland_surface);
        state_->surface_listener.configure = surface_configure;
        xdg_surface_add_listener(
            state_->xdg_surface_handle,
            &state_->surface_listener,
            state_.get());
        state_->xdg_toplevel_handle =
            xdg_surface_get_toplevel(state_->xdg_surface_handle);
        state_->toplevel_listener.configure = toplevel_configure;
        state_->toplevel_listener.close = toplevel_close;
        state_->toplevel_listener.configure_bounds =
            toplevel_configure_bounds;
        state_->toplevel_listener.wm_capabilities =
            toplevel_wm_capabilities;
        xdg_toplevel_add_listener(
            state_->xdg_toplevel_handle,
            &state_->toplevel_listener,
            state_.get());
        xdg_toplevel_set_title(
            state_->xdg_toplevel_handle, "Zevryon Vulkan WSI");
        xdg_surface_set_window_geometry(
            state_->xdg_surface_handle,
            0,
            0,
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height));
        wl_surface_commit(state_->wayland_surface);
        for (std::uint32_t round = 0U;
             round < 16U && !state_->wayland_configured;
             ++round) {
            if (wl_display_roundtrip(state_->wayland_display) < 0) {
                break;
            }
        }
        if (!state_->wayland_configured) {
            set_error(error, "Wayland xdg surface was not configured");
            destroy();
            return false;
        }
        state_->handle.generation = 1U;
        state_->handle.display_or_instance = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(state_->wayland_display));
        state_->handle.window_or_layer = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(state_->wayland_surface));
        state_->handle.system = system;
        return true;
    }
#endif
    set_error(error, "requested Linux test window system is unavailable");
    destroy();
    return false;
#else
    (void)system;
    set_error(error, "native test windows are unavailable");
    destroy();
    return false;
#endif
}

bool NativeVulkanTestWindow::resize(
    std::uint32_t width,
    std::uint32_t height,
    std::string* error) noexcept {
    if (state_ == nullptr || width == 0U || height == 0U) {
        set_error(error, "invalid test window resize");
        return false;
    }
#if defined(_WIN32)
    if (!SetWindowPos(
            state_->window,
            nullptr,
            0,
            0,
            static_cast<int>(width),
            static_cast<int>(height),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        set_error(error, "SetWindowPos failed");
        return false;
    }
#elif defined(__linux__)
    if (state_->system == NativeWindowSystem::Xcb) {
        const std::array<std::uint32_t, 2U> values{width, height};
        xcb_configure_window(
            state_->xcb_connection,
            state_->xcb_window,
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values.data());
        xcb_flush(state_->xcb_connection);
    }
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    else if (state_->system == NativeWindowSystem::Wayland) {
        xdg_surface_set_window_geometry(
            state_->xdg_surface_handle,
            0,
            0,
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height));
        wl_surface_commit(state_->wayland_surface);
        wl_display_flush(state_->wayland_display);
    }
#endif
#endif
    state_->width = width;
    state_->height = height;
    return pump(error);
}

bool NativeVulkanTestWindow::pump(std::string* error) noexcept {
    if (state_ == nullptr) {
        set_error(error, "test window is unavailable");
        return false;
    }
#if defined(_WIN32)
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#elif defined(__linux__)
    if (state_->system == NativeWindowSystem::Xcb) {
        while (xcb_generic_event_t* event =
                   xcb_poll_for_event(state_->xcb_connection)) {
            std::free(event);
        }
        xcb_flush(state_->xcb_connection);
    }
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    else if (state_->system == NativeWindowSystem::Wayland) {
        if (wl_display_dispatch_pending(state_->wayland_display) < 0 ||
            wl_display_flush(state_->wayland_display) < 0) {
            set_error(error, "Wayland event dispatch failed");
            return false;
        }
    }
#endif
#endif
    return true;
}

void NativeVulkanTestWindow::destroy() noexcept {
    if (state_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    if (state_->window != nullptr) {
        DestroyWindow(state_->window);
    }
#elif defined(__linux__)
    if (state_->xcb_connection != nullptr) {
        if (state_->xcb_window != 0U) {
            xcb_destroy_window(
                state_->xcb_connection, state_->xcb_window);
        }
        xcb_disconnect(state_->xcb_connection);
    }
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    if (state_->xdg_toplevel_handle != nullptr) {
        xdg_toplevel_destroy(state_->xdg_toplevel_handle);
    }
    if (state_->xdg_surface_handle != nullptr) {
        xdg_surface_destroy(state_->xdg_surface_handle);
    }
    if (state_->wayland_surface != nullptr) {
        wl_surface_destroy(state_->wayland_surface);
    }
    if (state_->wm_base != nullptr) {
        xdg_wm_base_destroy(state_->wm_base);
    }
    if (state_->wayland_compositor != nullptr) {
        wl_compositor_destroy(state_->wayland_compositor);
    }
    if (state_->wayland_registry != nullptr) {
        wl_registry_destroy(state_->wayland_registry);
    }
    if (state_->wayland_display != nullptr) {
        wl_display_disconnect(state_->wayland_display);
    }
#endif
#endif
    state_.reset();
}

NativeWindowSurfaceHandle NativeVulkanTestWindow::handle() const noexcept {
    return state_ != nullptr ? state_->handle : NativeWindowSurfaceHandle{};
}
std::uint32_t NativeVulkanTestWindow::width() const noexcept {
    return state_ != nullptr ? state_->width : 0U;
}
std::uint32_t NativeVulkanTestWindow::height() const noexcept {
    return state_ != nullptr ? state_->height : 0U;
}

} // namespace zevryon::text::test
