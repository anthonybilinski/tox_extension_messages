#include <toxext/toxext.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "tox_extension_messages.h"


static uint8_t const uuid[16] = {0x9e, 0x10, 0x03, 0x16, 0xd2, 0x6f, 0x45, 0x39, 0x8c, 0xdb, 0xae, 0x81, 0x00, 0x42, 0xf8, 0x64};

struct FriendState {
	uint32_t friend_id;
	uint8_t* next_message;
	size_t next_message_size;
};

enum Messages {
	MESSAGE_PART,
	MESSAGE_FINISH,
};

struct ToxExtensionMessages {
	struct ToxExtExtension* extension_handle;
	// Ideally we would use a better data structure for this but C doesn't have a ton available
	struct FriendState* friend_state;
	size_t friend_state_size;
	tox_extension_messages_received_cb cb;
	tox_extension_messages_negotiate_cb negotiated_cb;
	void* userdata;
};

static struct FriendState* get_friend_state(
		struct ToxExtensionMessages* extension,
		uint32_t friend_id) {
	for (size_t i = 0; i < extension->friend_state_size; ++i) {
		if (extension->friend_state[i].friend_id == friend_id) {
			return &extension->friend_state[i];
		}
	}

	return NULL;
}

static void init_friend_state(struct FriendState* friend_state, uint32_t friend_id) {
	friend_state->friend_id = friend_id;
	friend_state->next_message = NULL;
	friend_state->next_message_size = 0;
}

static struct FriendState* insert_friend_state(struct ToxExtensionMessages* extension, uint32_t friend_id) {
	struct FriendState* new_friend_state = realloc(
		extension->friend_state,
		(extension->friend_state_size + 1) * sizeof(struct FriendState));

	if (!new_friend_state) {
		return NULL;
	}

	extension->friend_state = new_friend_state;
	extension->friend_state_size++;

	struct FriendState* friend_state = &extension->friend_state[extension->friend_state_size - 1];
	init_friend_state(friend_state, friend_id);
	return friend_state;
}

static struct FriendState* get_or_insert_friend_state(
		struct ToxExtensionMessages* extension,
		uint32_t friend_id) {

	struct FriendState* friend_state = get_friend_state(extension, friend_id);

	if (!friend_state) {
		friend_state = insert_friend_state(extension, friend_id);
	}

	return friend_state;
}

// Here they confirm that they are indeed sending us messages. We don't call the
// negotation callback when we've both negotiated that we have the extension
// because at that point they don't know we can accept the message ids. We wait for
// an enable flag from the other side to indicate that they are now embedding message ids.
static void tox_extension_messages_recv(struct ToxExtExtension* extension, uint32_t friend_id, void const* data, size_t size, void* userdata, struct ToxExtPacketList* response_packet) {
	(void)extension;
	(void)response_packet;
	struct ToxExtensionMessages *ext_message_ids = userdata;
	struct FriendState* friend_state = get_friend_state(ext_message_ids, friend_id);

	uint8_t const* it = data;
	enum Messages message_type = *it;
	it++;

	switch (message_type) {
	case MESSAGE_PART:
	case MESSAGE_FINISH:
		break;
	default:
		return;
	}

	uint8_t const* message = it;
	size_t message_size = size - 1;
	size_t current_message_size = friend_state->next_message_size;

	uint8_t* resized_message = realloc(friend_state->next_message, current_message_size + message_size);

	if (!resized_message) {
		free(friend_state->next_message);
		friend_state->next_message = NULL;
		friend_state->next_message_size = 0;
		return;
	}
	else {
		friend_state->next_message = resized_message;
		friend_state->next_message_size = current_message_size + message_size;
	}

	memcpy(friend_state->next_message + current_message_size, message, message_size);

	if (message_type == MESSAGE_FINISH) {

		if (ext_message_ids->cb) {
			ext_message_ids->cb(friend_id, friend_state->next_message, friend_state->next_message_size, ext_message_ids->userdata);
		}
		free(friend_state->next_message);
		friend_state->next_message = NULL;
		friend_state->next_message_size = 0;
	}

}

static void tox_extension_messages_neg(struct ToxExtExtension* extension, uint32_t friend_id, bool compatible, void* userdata, struct ToxExtPacketList* response_packet) {
	(void)extension;
	(void)response_packet;
	struct ToxExtensionMessages *ext_message_ids = userdata;
	get_or_insert_friend_state(ext_message_ids, friend_id);
	ext_message_ids->negotiated_cb(friend_id, compatible, ext_message_ids->userdata);
}

struct ToxExtensionMessages* tox_extension_messages_register(
    struct ToxExt* toxext,
    tox_extension_messages_received_cb cb,
    tox_extension_messages_negotiate_cb neg_cb,
    void* userdata) {

	assert(cb);

	struct ToxExtensionMessages* extension = malloc(sizeof(struct ToxExtensionMessages));
	extension->extension_handle = toxext_register(toxext, uuid, extension, tox_extension_messages_recv, tox_extension_messages_neg);
	extension->friend_state = NULL;
	extension->friend_state_size = 0;
	extension->cb = cb;
	extension->negotiated_cb = neg_cb;
	extension->userdata = userdata;

	if (!extension->extension_handle) {
		free(extension);
		return NULL;
	}

	return extension;
}

void tox_extension_messages_free(struct ToxExtensionMessages* extension) {
	for (size_t i = 0; i < extension->friend_state_size; ++i) {
		free(extension->friend_state[i].next_message);
	}
	free(extension->friend_state);
	free(extension);
}

void tox_extension_messages_negotiate(struct ToxExtensionMessages* extension, uint32_t friend_id) {
	toxext_negotiate_connection(extension->extension_handle, friend_id);
}

void tox_extension_messages_append(struct ToxExtensionMessages* extension, struct ToxExtPacketList* packet, uint8_t const* data, size_t size) {
	size_t remaining_size = size;
	uint8_t const* remaining_data = data;

	do {
		uint8_t extension_data[TOXEXT_MAX_PACKET_SIZE];
		bool bLastChunk = remaining_size <= TOXEXT_MAX_PACKET_SIZE - 1;
		size_t const size_for_chunk = bLastChunk ? remaining_size : TOXEXT_MAX_PACKET_SIZE - 1;

		memcpy(extension_data + 1, remaining_data, size_for_chunk);
		remaining_size -= size_for_chunk;
		remaining_data += size_for_chunk;

		if (bLastChunk) {
			extension_data[0] = MESSAGE_FINISH;
		}
		else {
			extension_data[0] = MESSAGE_PART;
		}

		toxext_packet_append(packet, extension->extension_handle, extension_data, size_for_chunk + 1);

	} while (remaining_size > 0);
}
