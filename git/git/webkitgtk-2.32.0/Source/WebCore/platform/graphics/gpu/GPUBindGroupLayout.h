/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WEBGPU)

#include "GPUBindGroupLayoutDescriptor.h"
#include "GPUPlatformTypes.h"
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/Variant.h>

namespace WebCore {

class GPUDevice;

class GPUBindGroupLayout : public RefCounted<GPUBindGroupLayout> {
public:
    static RefPtr<GPUBindGroupLayout> tryCreate(const GPUDevice&, const GPUBindGroupLayoutDescriptor&);

    struct UniformBuffer {
        unsigned internalLengthName;
    };

    struct DynamicUniformBuffer {
        unsigned internalLengthName;
    };

    struct Sampler {
    };

    struct SampledTexture {
    };

    struct StorageBuffer {
        unsigned internalLengthName;
    };

    struct DynamicStorageBuffer {
        unsigned internalLengthName;
    };

    using InternalBindingDetails = Variant<UniformBuffer, DynamicUniformBuffer, Sampler, SampledTexture, StorageBuffer, DynamicStorageBuffer>;

    struct Binding {
        GPUBindGroupLayoutBinding externalBinding;
        unsigned internalName;
        InternalBindingDetails internalBindingDetails;
    };

    using BindingsMapType = HashMap<uint64_t, Binding, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    const BindingsMapType& bindingsMap() const { return m_bindingsMap; }
#if USE(METAL)
    MTLArgumentEncoder *vertexEncoder() const { return m_vertexEncoder.get(); }
    MTLArgumentEncoder *fragmentEncoder() const { return m_fragmentEncoder.get(); }
    MTLArgumentEncoder *computeEncoder() const { return m_computeEncoder.get(); }
#endif // USE(METAL)

private:
#if USE(METAL)
    GPUBindGroupLayout(BindingsMapType&&, RetainPtr<MTLArgumentEncoder>&& vertex, RetainPtr<MTLArgumentEncoder>&& fragment, RetainPtr<MTLArgumentEncoder>&& compute);

    RetainPtr<MTLArgumentEncoder> m_vertexEncoder;
    RetainPtr<MTLArgumentEncoder> m_fragmentEncoder;
    RetainPtr<MTLArgumentEncoder> m_computeEncoder;
#endif // USE(METAL)
    const BindingsMapType m_bindingsMap;
};

} // namespace WebCore

#endif // ENABLE(WEBGPU)