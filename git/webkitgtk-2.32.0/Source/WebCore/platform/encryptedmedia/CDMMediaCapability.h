/*
 * Copyright (C) 2017 Metrological Group B.V.
 * Copyright (C) 2017 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(ENCRYPTED_MEDIA)

#include "CDMEncryptionScheme.h"
#include <wtf/text/WTFString.h>

namespace WebCore {

struct CDMMediaCapability {
    String contentType;
    String robustness;
    Optional<CDMEncryptionScheme> encryptionScheme;

    template<class Encoder>
    void encode(Encoder& encoder) const
    {
        encoder << contentType;
        encoder << robustness;
        encoder << encryptionScheme;
    }

    template <class Decoder>
    static Optional<CDMMediaCapability> decode(Decoder& decoder)
    {
        Optional<String> contentType;
        decoder >> contentType;
        if (!contentType)
            return WTF::nullopt;

        Optional<String> robustness;
        decoder >> robustness;
        if (!robustness)
            return WTF::nullopt;

        Optional<Optional<CDMEncryptionScheme>> encryptionScheme;
        decoder >> encryptionScheme;
        if (!encryptionScheme)
            return WTF::nullopt;

        return {{
            WTFMove(*contentType),
            WTFMove(*robustness),
            WTFMove(*encryptionScheme),
        }};
    }
};

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA)