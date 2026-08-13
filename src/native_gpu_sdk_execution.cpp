#include "native_gpu_sdk_execution.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

void clear_error(NativeGpuSdkError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeGpuSdkErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool fail(
    NativeGpuSdkError* error,
    NativeGpuSdkErrorKind kind,
    const char* message,
    std::int64_t native_code = 0) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = native_code;
        try {
            error->message = message == nullptr ? "native GPU SDK failure" : message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_api_error(NativeGpuApiError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeGpuApiErrorKind::None;
        error->message.clear();
    }
}

bool fail_api(
    NativeGpuApiError* error,
    NativeGpuApiErrorKind kind,
    const NativeGpuSdkError& sdk_error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        try {
            error->message = sdk_error.message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_surface(const GpuSurfaceDescriptor& surface) noexcept {
    return surface.surface_id != 0U && surface.generation_id != 0U &&
           surface.width != 0U && surface.height != 0U;
}

bool valid_limits(const NativeGpuSdkLimits& limits) noexcept {
    return limits.maximum_swapchain_images > 0U &&
           limits.maximum_swapchain_images <= 16U &&
           limits.maximum_frames_in_flight > 0U &&
           limits.maximum_frames_in_flight <= 16U &&
           limits.maximum_command_allocators > 0U &&
           limits.maximum_command_allocators <= 32U &&
           limits.maximum_descriptors > 0U &&
           limits.maximum_texture_resources > 0U &&
           limits.maximum_staging_bytes > 0U &&
           limits.maximum_device_local_bytes > 0U &&
           limits.maximum_submission_commands > 0U;
}

NativePlatformCapabilities map_capabilities(
    const NativeGpuSdkProbe& probe,
    const NativeGpuSdkLimits& limits) noexcept {
    NativePlatformCapabilities output;
    if (probe.api_kind == NativeGpuApiKind::Metal ||
        probe.availability != NativeGpuSdkAvailability::RuntimeReady) {
        return output;
    }
    if ((probe.flags & kNativeGpuSdkTimelineFence) != 0U) {
        output.flags |= kNativePlatformTimelineFence;
    }
    output.flags |= kNativePlatformPartialPresent;
    if (probe.api_kind == NativeGpuApiKind::Vulkan) {
        output.flags |= kNativePlatformMailboxPresent |
                        kNativePlatformImmediatePresent |
                        kNativePlatformExplicitBarriers;
    } else if (probe.api_kind == NativeGpuApiKind::Direct3D12) {
        output.flags |= kNativePlatformImmediatePresent |
                        kNativePlatformTearing |
                        kNativePlatformExplicitBarriers;
    }
    output.maximum_commands = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        limits.maximum_submission_commands,
        std::numeric_limits<std::uint32_t>::max()));
    output.maximum_barriers = output.maximum_commands;
    output.maximum_descriptors = limits.maximum_descriptors;
    output.maximum_swapchain_images = limits.maximum_swapchain_images;
    output.maximum_frames_in_flight = limits.maximum_frames_in_flight;
    output.maximum_staging_bytes = limits.maximum_staging_bytes;
    return output;
}

NativeGpuApiErrorKind map_api_error_kind(NativeGpuSdkErrorKind kind) noexcept {
    switch (kind) {
        case NativeGpuSdkErrorKind::None:
            return NativeGpuApiErrorKind::None;
        case NativeGpuSdkErrorKind::InvalidInput:
        case NativeGpuSdkErrorKind::UnsupportedBackend:
        case NativeGpuSdkErrorKind::RuntimeUnavailable:
        case NativeGpuSdkErrorKind::StaleGeneration:
            return NativeGpuApiErrorKind::InvalidInput;
        case NativeGpuSdkErrorKind::SurfaceConfigurationFailed:
        case NativeGpuSdkErrorKind::ResourceAllocationFailed:
        case NativeGpuSdkErrorKind::ResourceBudgetExceeded:
            return NativeGpuApiErrorKind::SurfaceConfigurationFailed;
        case NativeGpuSdkErrorKind::AcquireFailed:
            return NativeGpuApiErrorKind::AcquireFailed;
        case NativeGpuSdkErrorKind::CommandEncodingFailed:
            return NativeGpuApiErrorKind::EncodeFailed;
        case NativeGpuSdkErrorKind::SubmissionFailed:
            return NativeGpuApiErrorKind::EncodeFailed;
        case NativeGpuSdkErrorKind::PresentFailed:
            return NativeGpuApiErrorKind::PresentFailed;
        case NativeGpuSdkErrorKind::FenceRegression:
        case NativeGpuSdkErrorKind::AggregateOverflow:
            return NativeGpuApiErrorKind::FenceOverflow;
        case NativeGpuSdkErrorKind::DeviceCreationFailed:
        case NativeGpuSdkErrorKind::QueueCreationFailed:
        case NativeGpuSdkErrorKind::DeviceLost:
            return NativeGpuApiErrorKind::DeviceLost;
    }
    return NativeGpuApiErrorKind::InvalidInput;
}

} // namespace

const char* native_gpu_sdk_error_kind_name(NativeGpuSdkErrorKind kind) noexcept {
    switch (kind) {
        case NativeGpuSdkErrorKind::None: return "none";
        case NativeGpuSdkErrorKind::InvalidInput: return "invalid_input";
        case NativeGpuSdkErrorKind::UnsupportedBackend: return "unsupported_backend";
        case NativeGpuSdkErrorKind::RuntimeUnavailable: return "runtime_unavailable";
        case NativeGpuSdkErrorKind::DeviceCreationFailed: return "device_creation_failed";
        case NativeGpuSdkErrorKind::QueueCreationFailed: return "queue_creation_failed";
        case NativeGpuSdkErrorKind::SurfaceConfigurationFailed: return "surface_configuration_failed";
        case NativeGpuSdkErrorKind::ResourceAllocationFailed: return "resource_allocation_failed";
        case NativeGpuSdkErrorKind::ResourceBudgetExceeded: return "resource_budget_exceeded";
        case NativeGpuSdkErrorKind::AcquireFailed: return "acquire_failed";
        case NativeGpuSdkErrorKind::CommandEncodingFailed: return "command_encoding_failed";
        case NativeGpuSdkErrorKind::SubmissionFailed: return "submission_failed";
        case NativeGpuSdkErrorKind::PresentFailed: return "present_failed";
        case NativeGpuSdkErrorKind::FenceRegression: return "fence_regression";
        case NativeGpuSdkErrorKind::DeviceLost: return "device_lost";
        case NativeGpuSdkErrorKind::StaleGeneration: return "stale_generation";
        case NativeGpuSdkErrorKind::AggregateOverflow: return "aggregate_overflow";
    }
    return "unknown";
}

bool NativeGpuSdkApi::export_context(
    NativeGpuSdkContextHandle* context,
    NativeGpuSdkError* error) noexcept {
    clear_error(error);
    if (context == nullptr) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "native GPU context output is null");
    }
    *context = {};
    return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                "native GPU context export is unavailable for this backend");
}

ReferenceNativeGpuSdkApi::ReferenceNativeGpuSdkApi(NativeGpuApiKind kind) noexcept
    : kind_(kind) {
    snapshot_.probe.api_kind = kind;
    if (kind == NativeGpuApiKind::Metal) {
        snapshot_.probe.availability = NativeGpuSdkAvailability::Unavailable;
        return;
    }
    snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
    snapshot_.probe.api_major = 1U;
    snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                            kNativeGpuSdkOffscreenSurface |
                            kNativeGpuSdkTimelineFence;
    snapshot_.probe.runtime_generation = 1U;
    std::uint64_t checksum = kFnvOffset;
    hash_value(&checksum, kind);
    hash_value(&checksum, snapshot_.probe.flags);
    snapshot_.probe.checksum = checksum;
}

NativeGpuApiKind ReferenceNativeGpuSdkApi::kind() const noexcept {
    return kind_;
}

NativeGpuSdkProbe ReferenceNativeGpuSdkApi::probe() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.probe;
}

bool ReferenceNativeGpuSdkApi::initialize(
    const NativeGpuSdkConfig& config,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (kind_ == NativeGpuApiKind::Metal) {
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Metal support was removed from Zevryon");
    }
    if (fail_initialization_) {
        return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                    "reference device initialization was forced to fail");
    }
    if (config.api_kind != kind_ || config.device_generation == 0U ||
        config.runtime_generation == 0U || !valid_limits(config.limits)) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "invalid native GPU SDK configuration");
    }
    if (config.window.system != NativeWindowSystem::Headless) {
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Z2F-8A accepts only the headless/offscreen surface bridge");
    }
    snapshot_.config = config;
    snapshot_.probe.runtime_generation = config.runtime_generation;
    snapshot_.initialized_devices += 1U;
    initialized_ = true;
    return true;
}

bool ReferenceNativeGpuSdkApi::configure_offscreen_surface(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (!initialized_) {
        return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                    "native GPU SDK device is not initialized");
    }
    if (!valid_surface(surface) || image_count == 0U ||
        image_count > snapshot_.config.limits.maximum_swapchain_images) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "invalid offscreen surface configuration");
    }
    const std::uint64_t bytes_per_image =
        static_cast<std::uint64_t>(surface.width) * surface.height * 4U;
    if (surface.width != 0U && bytes_per_image / surface.width / 4U != surface.height) {
        return fail(error, NativeGpuSdkErrorKind::AggregateOverflow,
                    "offscreen image byte count overflowed");
    }
    if (bytes_per_image > snapshot_.config.limits.maximum_device_local_bytes /
                              static_cast<std::uint64_t>(image_count)) {
        return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                    "offscreen image ring exceeds device-local budget");
    }
    snapshot_.surface = surface;
    snapshot_.configured_image_count = image_count;
    snapshot_.configured_surfaces += 1U;
    snapshot_.current_device_local_bytes = bytes_per_image * image_count;
    snapshot_.peak_device_local_bytes = std::max(
        snapshot_.peak_device_local_bytes,
        snapshot_.current_device_local_bytes);
    next_image_index_ = 0U;
    return true;
}

bool ReferenceNativeGpuSdkApi::acquire_image(
    const GpuSurfaceDescriptor& surface,
    std::uint64_t ticket_id,
    NativePlatformSwapchainImage* image,
    NativeAcquireStatus* status,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (image == nullptr || status == nullptr || ticket_id == 0U) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "invalid acquire output or ticket");
    }
    if (!initialized_ || snapshot_.configured_image_count == 0U) {
        return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                    "offscreen surface is not configured");
    }
    if (!(surface == snapshot_.surface)) {
        return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                    "surface generation changed before acquire");
    }
    *status = std::exchange(next_acquire_status_, NativeAcquireStatus::Acquired);
    if (*status != NativeAcquireStatus::Acquired) {
        if (*status == NativeAcquireStatus::DeviceLost) {
            snapshot_.device_lost_events += 1U;
        }
        return true;
    }
    const std::uint32_t index = next_image_index_++ % snapshot_.configured_image_count;
    image->image.device_generation = snapshot_.config.device_generation;
    image->image.surface_id = surface.surface_id;
    image->image.surface_generation = surface.generation_id;
    image->image.image_generation = next_image_generation_++;
    image->image.image_index = index;
    image->image.flags = 0U;
    image->driver_generation = snapshot_.config.runtime_generation;
    image->native_resource_id = next_resource_id_++;
    image->state = NativePlatformResourceState::Present;
    image->reserved = 0U;
    snapshot_.acquired_images += 1U;
    return true;
}

bool ReferenceNativeGpuSdkApi::execute_submission(
    const NativePlatformSubmission& submission,
    NativeGpuSdkSubmissionReceipt* receipt,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (receipt == nullptr || !initialized_) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "invalid submission receipt or uninitialized device");
    }
    if (submission.api_kind != kind_ ||
        !(submission.surface == snapshot_.surface) ||
        submission.image.image.device_generation != snapshot_.config.device_generation ||
        submission.image.image.surface_id != snapshot_.surface.surface_id ||
        submission.image.image.surface_generation != snapshot_.surface.generation_id ||
        submission.image.driver_generation != snapshot_.config.runtime_generation) {
        snapshot_.stale_rejections += 1U;
        return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                    "submission references a stale device, runtime or surface generation");
    }
    if (submission.commands.size() > snapshot_.config.limits.maximum_submission_commands ||
        submission.descriptors.size() > snapshot_.config.limits.maximum_descriptors) {
        return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                    "submission exceeds bounded SDK command or descriptor limits");
    }
    NativePresentStatus present_status =
        std::exchange(next_present_status_, NativePresentStatus::Presented);
    if (present_status == NativePresentStatus::DeviceLost) {
        snapshot_.device_lost_events += 1U;
        return fail(error, NativeGpuSdkErrorKind::DeviceLost,
                    "reference device was lost during submission");
    }
    std::uint64_t checksum = kFnvOffset;
    hash_value(&checksum, submission.encoded_checksum);
    hash_value(&checksum, submission.frame_id);
    hash_value(&checksum, submission.ticket_id);
    hash_value(&checksum, submission.commands.size());
    hash_value(&checksum, submission.barriers.size());
    hash_value(&checksum, submission.descriptors.size());
    hash_value(&checksum, kind_);
    const std::uint64_t signal = next_fence_value_++;
    if (signal <= snapshot_.last_submitted_fence_value) {
        return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                    "reference SDK fence timeline regressed");
    }
    receipt->api_kind = kind_;
    receipt->status = present_status;
    receipt->command_count = static_cast<std::uint32_t>(submission.commands.size());
    receipt->barrier_count = static_cast<std::uint32_t>(submission.barriers.size());
    receipt->descriptor_count = static_cast<std::uint32_t>(submission.descriptors.size());
    receipt->image_index = submission.image.image.image_index;
    receipt->device_generation = snapshot_.config.device_generation;
    receipt->runtime_generation = snapshot_.config.runtime_generation;
    receipt->surface_generation = submission.surface.generation_id;
    receipt->frame_id = submission.frame_id;
    receipt->ticket_id = submission.ticket_id;
    receipt->wait_fence_value = submission.wait_fence_value;
    receipt->signal_fence_value = signal;
    receipt->encoded_checksum = checksum;
    snapshot_.submitted_frames += 1U;
    snapshot_.in_flight_frame_count += 1U;
    snapshot_.last_submitted_fence_value = signal;
    return true;
}

bool ReferenceNativeGpuSdkApi::retire_completed(
    std::uint64_t completed_fence_value,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (completed_fence_value < snapshot_.completed_fence_value ||
        completed_fence_value > snapshot_.last_submitted_fence_value) {
        return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                    "completion fence is outside the submitted timeline");
    }
    if (completed_fence_value > snapshot_.completed_fence_value) {
        const std::uint64_t newly_retired =
            completed_fence_value - snapshot_.completed_fence_value;
        const std::uint64_t bounded_retired = std::min<std::uint64_t>(
            newly_retired, snapshot_.in_flight_frame_count);
        snapshot_.retired_frames += bounded_retired;
        snapshot_.in_flight_frame_count -= static_cast<std::uint32_t>(bounded_retired);
        snapshot_.completed_fence_value = completed_fence_value;
    }
    return true;
}

NativeGpuSdkSnapshot ReferenceNativeGpuSdkApi::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void ReferenceNativeGpuSdkApi::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.surface = {};
    snapshot_.configured_image_count = 0U;
    snapshot_.in_flight_frame_count = 0U;
    snapshot_.current_staging_bytes = 0U;
    snapshot_.current_device_local_bytes = 0U;
    snapshot_.last_submitted_fence_value = 0U;
    snapshot_.completed_fence_value = 0U;
    next_image_index_ = 0U;
    initialized_ = false;
}

void ReferenceNativeGpuSdkApi::set_next_acquire_status(NativeAcquireStatus status) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    next_acquire_status_ = status;
}

void ReferenceNativeGpuSdkApi::set_next_present_status(NativePresentStatus status) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    next_present_status_ = status;
}

void ReferenceNativeGpuSdkApi::set_fail_initialization(bool fail_initialization) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_initialization_ = fail_initialization;
}

NativeGpuSdkPlatformDriver::NativeGpuSdkPlatformDriver(
    NativeGpuSdkApi* api,
    NativeGpuSdkConfig config) noexcept
    : api_(api), config_(config) {
    if (api_ != nullptr) {
        const NativeGpuSdkProbe sdk_probe = api_->probe();
        capabilities_ = map_capabilities(sdk_probe, config_.limits);
    }
}

NativeGpuApiKind NativeGpuSdkPlatformDriver::kind() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return api_ == nullptr ? NativeGpuApiKind::ReferenceCpu : api_->kind();
}

NativePlatformCapabilities NativeGpuSdkPlatformDriver::capabilities() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return capabilities_;
}

bool NativeGpuSdkPlatformDriver::configure_swapchain(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    const NativePlatformAdapterConfig& adapter_config,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_api_error(error);
    if (api_ == nullptr || adapter_config.api_kind != config_.api_kind ||
        adapter_config.device_generation != config_.device_generation ||
        adapter_config.driver_generation != config_.runtime_generation) {
        NativeGpuSdkError sdk_error;
        fail(&sdk_error, NativeGpuSdkErrorKind::InvalidInput,
             "native SDK driver configuration does not match adapter generations");
        return fail_api(error, NativeGpuApiErrorKind::InvalidInput, sdk_error);
    }
    NativeGpuSdkError sdk_error;
    if (!initialized_) {
        if (!api_->initialize(config_, &sdk_error)) {
            return fail_api(error, map_api_error_kind(sdk_error.kind), sdk_error);
        }
        initialized_ = true;
        const NativeGpuSdkProbe sdk_probe = api_->probe();
        capabilities_ = map_capabilities(sdk_probe, config_.limits);
    }
    if (!api_->configure_offscreen_surface(surface, image_count, &sdk_error)) {
        return fail_api(error, map_api_error_kind(sdk_error.kind), sdk_error);
    }
    surface_ = surface;
    configured_ = true;
    return true;
}

bool NativeGpuSdkPlatformDriver::acquire_image(
    const GpuSurfaceDescriptor& surface,
    NativePresentMode,
    std::uint64_t ticket_id,
    NativePlatformSwapchainImage* image,
    NativeAcquireStatus* status,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_api_error(error);
    if (api_ == nullptr || !configured_ || !(surface == surface_)) {
        NativeGpuSdkError sdk_error;
        fail(&sdk_error, NativeGpuSdkErrorKind::StaleGeneration,
             "native SDK surface is not configured or changed generation");
        return fail_api(error, NativeGpuApiErrorKind::AcquireFailed, sdk_error);
    }
    NativeGpuSdkError sdk_error;
    if (!api_->acquire_image(surface, ticket_id, image, status, &sdk_error)) {
        return fail_api(error, map_api_error_kind(sdk_error.kind), sdk_error);
    }
    return true;
}

bool NativeGpuSdkPlatformDriver::submit_and_present(
    const NativePlatformSubmission& submission,
    std::uint64_t* signal_fence_value,
    std::uint64_t* encoded_checksum,
    NativePresentStatus* status,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_api_error(error);
    if (api_ == nullptr || signal_fence_value == nullptr ||
        encoded_checksum == nullptr || status == nullptr || !configured_) {
        NativeGpuSdkError sdk_error;
        fail(&sdk_error, NativeGpuSdkErrorKind::InvalidInput,
             "invalid native SDK submit outputs or unconfigured driver");
        return fail_api(error, NativeGpuApiErrorKind::EncodeFailed, sdk_error);
    }
    NativeGpuSdkSubmissionReceipt receipt;
    NativeGpuSdkError sdk_error;
    if (!api_->execute_submission(submission, &receipt, &sdk_error)) {
        return fail_api(error, map_api_error_kind(sdk_error.kind), sdk_error);
    }
    *signal_fence_value = receipt.signal_fence_value;
    *encoded_checksum = receipt.encoded_checksum;
    *status = receipt.status;
    return true;
}

bool NativeGpuSdkPlatformDriver::retire_completed(
    std::uint64_t completed_fence_value,
    NativeGpuSdkError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (api_ == nullptr) {
        return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                    "native SDK driver has no API");
    }
    return api_->retire_completed(completed_fence_value, error);
}

NativeGpuSdkSnapshot NativeGpuSdkPlatformDriver::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return api_ == nullptr ? NativeGpuSdkSnapshot{} : api_->snapshot();
}

void NativeGpuSdkPlatformDriver::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (api_ != nullptr) {
        api_->shutdown();
    }
    surface_ = {};
    configured_ = false;
    initialized_ = false;
}

NativeGpuSdkLimits default_native_gpu_sdk_limits(NativeGpuApiKind kind) noexcept {
    NativeGpuSdkLimits limits{};
    if (kind == NativeGpuApiKind::Metal) {
        return limits;
    }
    limits.maximum_swapchain_images = 3U;
    limits.maximum_frames_in_flight = 2U;
    limits.maximum_command_allocators = 3U;
    limits.maximum_descriptors = 512U;
    limits.maximum_texture_resources = 256U;
    limits.maximum_staging_bytes = 4U * 1024U * 1024U;
    limits.maximum_device_local_bytes = 64U * 1024U * 1024U;
    limits.maximum_submission_commands = 4096U;
    return limits;
}

} // namespace zevryon::text