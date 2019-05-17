#pragma once

#include <toxext/toxext.h>

struct ToxExtensionMessages;

typedef void(*tox_extension_messages_received_cb)(uint32_t friend_number, const uint8_t *message, size_t length, void *user_data);
typedef void(*tox_extension_messages_negotiate_cb)(uint32_t friend_number, bool negotiated, void *user_data);
struct ToxExtensionMessages* tox_extension_messages_register(
    struct ToxExt* toxext,
    tox_extension_messages_received_cb cb,
    tox_extension_messages_negotiate_cb neg_cb,
    void* userdata);
void tox_extension_messages_free(struct ToxExtensionMessages* extension);

void tox_extension_messages_negotiate(struct ToxExtensionMessages* extension, uint32_t friend_id);
void tox_extension_messages_append(struct ToxExtensionMessages* extension, struct ToxExtPacketList* packet, uint8_t const* data, size_t size);
