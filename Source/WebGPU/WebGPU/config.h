/*
 * Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H && defined(BUILDING_WITH_CMAKE)
#include "cmakeconfig.h"
#endif

#include "ExportMacros.h"

#include "WebGPU.h"

// The handles of webgpu.h point to the objects of the C++ API (see WebGPUCppBridge.h), never to
// these structs. Defining them keeps the structs of webgpu.h, and the code that passes the
// handles, from using forward declared types.
struct WGPUAdapterImpl { };
struct WGPUBindGroupImpl { };
struct WGPUBindGroupLayoutImpl { };
struct WGPUBufferImpl { };
struct WGPUCommandBufferImpl { };
struct WGPUCommandEncoderImpl { };
struct WGPUComputePassEncoderImpl { };
struct WGPUComputePipelineImpl { };
struct WGPUDeviceImpl { };
struct WGPUExternalTextureImpl { };
struct WGPUInstanceImpl { };
struct WGPUPipelineLayoutImpl { };
struct WGPUQuerySetImpl { };
struct WGPUQueueImpl { };
struct WGPURenderBundleImpl { };
struct WGPURenderBundleEncoderImpl { };
struct WGPURenderPassEncoderImpl { };
struct WGPURenderPipelineImpl { };
struct WGPUResourceTableImpl { };
struct WGPUSamplerImpl { };
struct WGPUShaderModuleImpl { };
struct WGPUSharedBufferMemoryImpl { };
struct WGPUSharedFenceImpl { };
struct WGPUSharedTextureMemoryImpl { };
struct WGPUSurfaceImpl { };
struct WGPUTexelBufferViewImpl { };
struct WGPUTextureImpl { };
struct WGPUTextureViewImpl { };

#include <Metal/Metal.h>

#include <wtf/Assertions.h>

#include <wtf/Platform.h>

#include <wtf/RetainPtr.h>

#if !PLATFORM(COCOA)
// The backend as part of another port on Darwin. WTF describes the system for the Cocoa ports:
// here its SPI headers declare nothing, PlatformHave.h does not know the SDK, and its logging
// formats with printf.

// The public headers of the macOS SDK have what <wtf/spi/cocoa/IOSurfaceSPI.h> gives a Cocoa port.
#include <IOSurface/IOSurface.h>
#ifdef __OBJC__
#import <IOSurface/IOSurfaceObjC.h>
#endif

#define HAVE_METAL_FAMILY_8 1
#define HAVE_METAL_FAMILY_9 1

// The port presents itself as the system's WebKit, whose WebGPU is older than this backend. Where
// a page can tell the two apart, the backend does what that one does: it has no subgroups, dual
// source blending or texture component swizzle, its limit of dynamic uniform buffers is the
// large one, an adapter gives any number of devices, and a lost device has no message.
#define USE_SYSTEM_WEBKIT_WEBGPU_PARITY 1

#ifdef __OBJC__
// The backend's messages carry Objective-C objects (%@), so Foundation formats them.
#define WTFLogAlways(format, ...) WTFLogAlways("%s", [NSString stringWithFormat:@format, ##__VA_ARGS__].UTF8String)
#endif
#endif // !PLATFORM(COCOA)
