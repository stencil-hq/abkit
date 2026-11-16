/*
 * Copyright (C) 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS AS IS''
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

#include "config.h"
#include "WebContextMenuProxyWPE.h"

#if ENABLE(CONTEXT_MENUS)

#include "WebContextMenuItem.h"
#include <wtf/text/CString.h>

namespace WebKit {
using namespace WebCore;

WebContextMenuProxyWPE::WebContextMenuProxyWPE(WebPageProxy& page, FrameInfoData&& frameInfo, ContextMenuContextData&& context, const UserData& userData)
    : WebContextMenuProxy(page, WTF::move(frameInfo), WTF::move(context), userData)
{
}

WebContextMenuProxyWPE::~WebContextMenuProxyWPE()
{
#if ENABLE(WPE_PLATFORM)
    if (m_activatedHandler)
        g_signal_handler_disconnect(m_view.get(), m_activatedHandler);
#endif
}

#if ENABLE(WPE_PLATFORM)
// The items as WPEView::context-menu describes them; an item's id is its action, which is unique in a menu.
static GVariant* itemsVariant(const Vector<WebContextMenuItemData>& items)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));
    for (const auto& item : items) {
        GVariantBuilder entry;
        g_variant_builder_init(&entry, G_VARIANT_TYPE_VARDICT);
        if (item.type() == ContextMenuItemType::Separator)
            g_variant_builder_add(&entry, "{sv}", "separator", g_variant_new_boolean(TRUE));
        else {
            g_variant_builder_add(&entry, "{sv}", "id", g_variant_new_uint32(static_cast<uint32_t>(item.action())));
            g_variant_builder_add(&entry, "{sv}", "label", g_variant_new_string(item.title().utf8().legacyCStringPointer()));
            g_variant_builder_add(&entry, "{sv}", "enabled", g_variant_new_boolean(item.enabled()));
            g_variant_builder_add(&entry, "{sv}", "checked", g_variant_new_boolean(item.checked()));
            g_variant_builder_add(&entry, "{sv}", "checkable", g_variant_new_boolean(item.type() == ContextMenuItemType::CheckableAction));
            if (item.type() == ContextMenuItemType::Submenu)
                g_variant_builder_add(&entry, "{sv}", "submenu", itemsVariant(item.submenu()));
        }
        g_variant_builder_add_value(&builder, g_variant_builder_end(&entry));
    }
    return g_variant_builder_end(&builder);
}

static const WebContextMenuItemData* findItem(const Vector<WebContextMenuItemData>& items, unsigned id)
{
    for (const auto& item : items) {
        if (item.type() == ContextMenuItemType::Submenu) {
            if (auto* found = findItem(item.submenu(), id))
                return found;
        } else if (item.type() != ContextMenuItemType::Separator && static_cast<unsigned>(item.action()) == id)
            return &item;
    }
    return nullptr;
}
#endif

// The menu goes to the platform the page's view is on (WPEView::context-menu), which shows it and says which
// item was chosen. Without the platform API there is nothing to show it.
void WebContextMenuProxyWPE::showContextMenuWithItems(Vector<Ref<WebContextMenuItem>>&& items)
{
#if ENABLE(WPE_PLATFORM)
    RefPtr page = this->page();
    auto* view = page ? page->wpeView() : nullptr;
    if (!view || items.isEmpty())
        return;
    m_items = WTF::map(items, [](auto& item) {
        return item->data();
    });
    m_view = view;
    m_activatedHandler = g_signal_connect(view, "context-menu-item-activated", G_CALLBACK(+[](WPEView*, guint id, gpointer proxy) {
        static_cast<WebContextMenuProxyWPE*>(proxy)->itemActivated(id);
    }), this);

    GVariantBuilder menu;
    g_variant_builder_init(&menu, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&menu, "{sv}", "x", g_variant_new_double(menuLocation().x()));
    g_variant_builder_add(&menu, "{sv}", "y", g_variant_new_double(menuLocation().y()));
    g_variant_builder_add(&menu, "{sv}", "items", itemsVariant(m_items));
    gboolean shown = FALSE;
    g_signal_emit_by_name(view, "context-menu", g_variant_builder_end(&menu), &shown);
#else
    UNUSED_PARAM(items);
#endif
}

#if ENABLE(WPE_PLATFORM)
void WebContextMenuProxyWPE::itemActivated(unsigned id)
{
    RefPtr page = this->page();
    auto* item = findItem(m_items, id);
    if (!page || !item || page->activeContextMenu() != this)
        return;
    Ref protectedThis { *this };
    page->contextMenuItemSelected(*item, frameInfo());
}
#endif

} // namespace WebKit
#endif // ENABLE(CONTEXT_MENUS)
