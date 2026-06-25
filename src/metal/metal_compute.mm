#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "melkor/metal_compute.hpp"
#include <string>

namespace melkor {
namespace metal {

class MetalContext::Impl {
public:
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    id<MTLLibrary> library = nil;
    bool initialized = false;
    
    ~Impl() {
        // ARC handles release
        library = nil;
        commandQueue = nil;
        device = nil;
    }
    
    bool loadLibrary() {
        if (!device) return false;
        
        NSError* error = nil;
        
        // Try to load from default.metallib in executable directory
        NSBundle* mainBundle = [NSBundle mainBundle];
        NSString* libraryPath = [mainBundle pathForResource:@"default" ofType:@"metallib"];
        
        if (libraryPath) {
            NSURL* libraryURL = [NSURL fileURLWithPath:libraryPath];
            library = [device newLibraryWithURL:libraryURL error:&error];
        }
        
        // If not found, try current working directory
        if (!library) {
            NSFileManager* fm = [NSFileManager defaultManager];
            NSString* cwd = [fm currentDirectoryPath];
            libraryPath = [cwd stringByAppendingPathComponent:@"default.metallib"];
            
            if ([fm fileExistsAtPath:libraryPath]) {
                NSURL* libraryURL = [NSURL fileURLWithPath:libraryPath];
                library = [device newLibraryWithURL:libraryURL error:&error];
            }
        }
        
        // Try executable path
        if (!library) {
            NSString* execPath = [[NSBundle mainBundle] executablePath];
            NSString* execDir = [execPath stringByDeletingLastPathComponent];
            libraryPath = [execDir stringByAppendingPathComponent:@"default.metallib"];
            
            NSFileManager* fm = [NSFileManager defaultManager];
            if ([fm fileExistsAtPath:libraryPath]) {
                NSURL* libraryURL = [NSURL fileURLWithPath:libraryPath];
                library = [device newLibraryWithURL:libraryURL error:&error];
            }
        }
        
        if (!library && error) {
            NSLog(@"Failed to load Metal library: %@", error);
        }
        
        return library != nil;
    }
};

MetalContext::MetalContext() : impl_(std::make_unique<Impl>()) {}
MetalContext::~MetalContext() = default;

bool MetalContext::isAvailable() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device != nil;
}

bool MetalContext::initialize() {
    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device) {
        return false;
    }
    
    impl_->commandQueue = [impl_->device newCommandQueue];
    if (!impl_->commandQueue) {
        impl_->device = nil;
        return false;
    }
    
    if (!impl_->loadLibrary()) {
        // Device and queue are valid, but without shaders no GPU operations
        // can run. Report failure so callers fall back to CPU.
        impl_->commandQueue = nil;
        impl_->device = nil;
        return false;
    }
    impl_->initialized = true;
    return true;
}

bool MetalContext::initialize(const std::string& device_name) {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    
    for (id<MTLDevice> dev in devices) {
        NSString* name = [dev name];
        if ([name containsString:[NSString stringWithUTF8String:device_name.c_str()]]) {
            impl_->device = dev;
            break;
        }
    }
    
    if (!impl_->device) {
        // Fall back to default
        impl_->device = MTLCreateSystemDefaultDevice();
    }
    
    if (!impl_->device) {
        return false;
    }
    
    impl_->commandQueue = [impl_->device newCommandQueue];
    if (!impl_->commandQueue) {
        impl_->device = nil;
        return false;
    }
    
    if (!impl_->loadLibrary()) {
        impl_->commandQueue = nil;
        impl_->device = nil;
        return false;
    }
    impl_->initialized = true;
    return true;
}

DeviceInfo MetalContext::getDeviceInfo() const {
    DeviceInfo info;
    
    if (impl_->device) {
        info.name = [[impl_->device name] UTF8String];
        info.recommended_max_working_set_size = [impl_->device recommendedMaxWorkingSetSize];
        info.max_threads_per_threadgroup = (uint32_t)[impl_->device maxThreadsPerThreadgroup].width;
        info.supports_family_apple7 = [impl_->device supportsFamily:MTLGPUFamilyApple7];
    }
    
    return info;
}

bool MetalContext::isInitialized() const {
    return impl_->initialized;
}

void* MetalContext::getDevice() const {
    return (__bridge void*)impl_->device;
}

void* MetalContext::getCommandQueue() const {
    return (__bridge void*)impl_->commandQueue;
}

void* MetalContext::getLibrary() const {
    return (__bridge void*)impl_->library;
}

} // namespace metal
} // namespace melkor
