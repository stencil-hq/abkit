/*
 * Copyright (C) 2006-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2007-2009 Torch Mobile, Inc.
 * Copyright (C) 2010, 2011 Research In Motion Limited. All rights reserved.
 * Copyright (C) 2013 Samsung Electronics. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#ifndef WTF_PLATFORM_GUARD_AGAINST_INDIRECT_INCLUSION
#error "Please #include <wtf/Platform.h> instead of this file directly."
#endif

#if !PLATFORM(GTK) && !PLATFORM(WPE)
#error "This file should only be included when building for the GTK or WPE platforms."
#endif

/* Please keep the following in alphabetical order so we can notice duplicates. */
/* Items should only be here if they are different from the defaults in PlatformEnable.h. */

#if !defined(ENABLE_COOKIE_STORE_API_BY_DEFAULT)
#define ENABLE_COOKIE_STORE_API_BY_DEFAULT 1
#endif

#if !defined(ENABLE_DESTINATION_COLOR_SPACE_DISPLAY_P3)
#define ENABLE_DESTINATION_COLOR_SPACE_DISPLAY_P3 1
#endif

#if !defined(ENABLE_KINETIC_SCROLLING) && (ENABLE(ASYNC_SCROLLING) || PLATFORM(GTK))
#define ENABLE_KINETIC_SCROLLING 1
#endif

#if !defined(ENABLE_NOTIFICATION_EVENT) && ENABLE(NOTIFICATIONS)
#define ENABLE_NOTIFICATION_EVENT 1
#endif

#if !defined(ENABLE_OPENTYPE_VERTICAL)
#define ENABLE_OPENTYPE_VERTICAL 1
#endif

#if !defined(ENABLE_SERVER_PRECONNECT)
#define ENABLE_SERVER_PRECONNECT 1
#endif

#if !defined(ENABLE_SCROLLING_THREAD) && USE(COORDINATED_GRAPHICS)
#define ENABLE_SCROLLING_THREAD 1
#endif

#if !defined(ENABLE_PDFJS)
#define ENABLE_PDFJS 1
#endif

#if !defined(ENABLE_WEBPROCESS_CACHE)
#define ENABLE_WEBPROCESS_CACHE 1
#endif

#if !defined(ENABLE_WHEEL_EVENT_REGIONS)
#define ENABLE_WHEEL_EVENT_REGIONS 1
#endif

#if !defined(ENABLE_COORDINATED_TOUCH_EVENTS) && ENABLE(TOUCH_EVENTS)
#define ENABLE_COORDINATED_TOUCH_EVENTS 1
#endif

#if !defined(ENABLE_TOUCH_EVENT_REGIONS) && ENABLE(COORDINATED_TOUCH_EVENTS)
#define ENABLE_TOUCH_EVENT_REGIONS 1
#endif

#if ENABLE(WPE_PLATFORM) || PLATFORM(GTK)
#define ENABLE_DAMAGE_TRACKING 1
#endif
#if !defined(ENABLE_AUTOCORRECT) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_AUTOCORRECT 1
#endif

#if !defined(ENABLE_WIRELESS_PLAYBACK_TARGET_AVAILABILITY_API) && ENABLE(WIRELESS_PLAYBACK_TARGET) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_WIRELESS_PLAYBACK_TARGET_AVAILABILITY_API 1
#endif

#if !defined(ENABLE_WRITING_SUGGESTIONS) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_WRITING_SUGGESTIONS 1
#endif

/* navigator.audioSession, exposed as Safari exposes it. The WPE port on Darwin has no
   system audio session, so the base WebCore AudioSession answers. */
#if !defined(ENABLE_DOM_AUDIO_SESSION) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_DOM_AUDIO_SESSION 1
#endif

/* window.ApplePayError, exposed as Safari exposes it: Safari's WebKit has Apple Pay built
   in and its other interfaces are gated by the ApplePayEnabled setting, so the error type is
   visible while no payment can be made. The WPE port has no Apple Pay implementation at all,
   so only this plain data type is built. */
#if !defined(ENABLE_APPLE_PAY_ERROR) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_APPLE_PAY_ERROR 1
#endif

/* window.pushManager, exposed as Safari exposes it. Safari's WebKit answers the calls with a
   push service; the whole declarative push feature also carries the Cocoa port's web push
   daemon client into the network process, which is no part of this engine, so the port builds
   the window half alone and the window answers the calls itself. */
#if !defined(ENABLE_WINDOW_PUSH_MANAGER) && PLATFORM(WPE) && OS(DARWIN)
#define ENABLE_WINDOW_PUSH_MANAGER 1
#endif
