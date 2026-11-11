/*
 * Copyright (C) 2026 Stencil
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

#include <JavaScriptCore/JSCJSValue.h>
#include <wtf/Forward.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>

namespace WebCore {

class SafariRemoteNotificationPermissionCallback;
struct SafariRemoteNotificationPermission;

// `window.safari.pushNotification`. No push service backs it, so no website ever holds a permission.
class SafariRemoteNotification : public RefCounted<SafariRemoteNotification> {
public:
    // Creates the object behind one window's `safari.pushNotification`.
    static Ref<SafariRemoteNotification> create() { return adoptRef(*new SafariRemoteNotification); }

    // Permission of the website push ID: always "default", without a device token.
    SafariRemoteNotificationPermission permission(const String& websitePushID) const;

    // Asks nobody; a task on the callback's context reports "denied" to it.
    void requestPermission(const String& webServiceURL, const String& websitePushID, JSC::JSValue userInfo, RefPtr<SafariRemoteNotificationPermissionCallback>&&);

private:
    SafariRemoteNotification() = default;
};

} // namespace WebCore
