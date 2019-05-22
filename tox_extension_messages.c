#include <toxext/toxext.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "tox_extension_messages.h"


static uint8_t const uuid[16] = {0x9e, 0x10, 0x03, 0x16, 0xd2, 0x6f, 0x45, 0x39, 0x8c, 0xdb, 0xae, 0x81, 0x00, 0x42, 0xf8, 0x64};

static uint32_t receipt_num = 0;

struct IncomingMessage {
	uint32_t friend_id;
	uint8_t* message;
	size_t size;
	size_t capacity;
	uint32_t receipt_num;
};

enum Messages {
	MESSAGE_START,
	MESSAGE_PART,
	MESSAGE_FINISH,
	MESSAGE_RECEIPT,
};

struct ToxExtensionMessages {
	struct ToxExtExtension* extension_handle;
	// Ideally we would use a better data structure for this but C doesn't have a ton available
	struct IncomingMessage* incoming_messages;
	size_t incoming_messages_size;
	tox_extension_messages_received_cb cb;
	tox_extension_messages_receipt_cb receipt_cb;
	tox_extension_messages_negotiate_cb negotiated_cb;
	void* userdata;
};

static struct IncomingMessage* get_incoming_message(
		struct ToxExtensionMessages* extension,
		uint32_t friend_id) {

	for (size_t i = 0; i < extension->incoming_messages_size; ++i) {
		if (extension->incoming_messages[i].friend_id == friend_id) {
			return &extension->incoming_messages[i];
		}
	}

	return NULL;
}

static void init_incoming_message(struct IncomingMessage* incoming_message, uint32_t friend_id) {
	incoming_message->friend_id = friend_id;
	incoming_message->message = NULL;
	incoming_message->size = 0;
	incoming_message->capacity = 0;
}

static struct IncomingMessage* insert_incoming_message(struct ToxExtensionMessages* extension, uint32_t friend_id) {
	struct IncomingMessage* new_incoming_messages = realloc(
		extension->incoming_messages,
		(extension->incoming_messages_size + 1) * sizeof(struct IncomingMessage));

	if (!new_incoming_messages) {
		return NULL;
	}

	extension->incoming_messages = new_incoming_messages;
	extension->incoming_messages_size++;

	struct IncomingMessage* incoming_message = &extension->incoming_messages[extension->incoming_messages_size - 1];
	init_incoming_message(incoming_message, friend_id);
	return incoming_message;
}

static struct IncomingMessage* get_or_insert_incoming_message(
		struct ToxExtensionMessages* extension,
		uint32_t friend_id) {

	struct IncomingMessage* incoming_message = get_incoming_message(extension, friend_id);

	if (!incoming_message) {
		incoming_message = insert_incoming_message(extension, friend_id);
	}

	return incoming_message;
}

static void clear_incoming_message(struct IncomingMessage* incoming_message) {
	free(incoming_message->message);
	incoming_message->message = NULL;
	incoming_message->size = 0;
	incoming_message->capacity = 0;
}

struct MessagesPacket {
	enum Messages message_type;
	/* On start packets we flag how large the entire buffer will be */
	size_t total_message_size;
	uint8_t const* message_data;
	size_t message_size;
	uint32_t receipt_num;
};

bool parse_messages_packet(
	uint8_t const* data,
	size_t size,
	struct MessagesPacket* messages_packet) {

	uint8_t const* it = data;
	uint8_t const* end = data + size;

	if (it + 1 > end) {
		return false;
	}
	messages_packet->message_type = *it;
	it += 1;

	if (messages_packet->message_type == MESSAGE_RECEIPT) {
		if (it + 4 > end) {
			return false;
		}
		messages_packet->receipt_num = 0;
		messages_packet->receipt_num |= (uint32_t)*(it + 0) << 24;
		messages_packet->receipt_num |= (uint32_t)*(it + 1) << 16;
		messages_packet->receipt_num |= (uint32_t)*(it + 2) << 8;
		messages_packet->receipt_num |= (uint32_t)*(it + 3) << 0;
		return true;
	}

	if (messages_packet->message_type == MESSAGE_START) {
		if (it + 8 > end) {
			return false;
		}

		/* fixme add read/write uint64_t methods */
		messages_packet->total_message_size = 0;
		messages_packet->total_message_size |= (uint64_t)*(it + 0) << 56;
		messages_packet->total_message_size |= (uint64_t)*(it + 1) << 48;
		messages_packet->total_message_size |= (uint64_t)*(it + 2) << 40;
		messages_packet->total_message_size |= (uint64_t)*(it + 3) << 36;
		messages_packet->total_message_size |= (uint64_t)*(it + 4) << 24;
		messages_packet->total_message_size |= (uint64_t)*(it + 5) << 16;
		messages_packet->total_message_size |= (uint64_t)*(it + 6) << 8;
		messages_packet->total_message_size |= (uint64_t)*(it + 7) << 0;

		messages_packet->receipt_num = 0;
		messages_packet->receipt_num |= (uint32_t)*(it + 8) << 24;
		messages_packet->receipt_num |= (uint32_t)*(it + 9) << 16;
		messages_packet->receipt_num |= (uint32_t)*(it + 10) << 8;
		messages_packet->receipt_num |= (uint32_t)*(it + 11) << 0;
		it += 12;
	}

	if (it > end) {
		return false;
	}

	messages_packet->message_data = it;
	messages_packet->message_size = end - it;

	return true;
}

// Here they confirm that they are indeed sending us messages. We don't call the
// negotation callback when we've both negotiated that we have the extension
// because at that point they don't know we can accept the message ids. We wait for
// an enable flag from the other side to indicate that they are now embedding message ids.
static void tox_extension_messages_recv(struct ToxExtExtension* extension, uint32_t friend_id, void const* data, size_t size, void* userdata, struct ToxExtPacketList* response_packet) {
	struct ToxExtensionMessages *ext_message_ids = userdata;
	struct IncomingMessage* incoming_message = get_incoming_message(ext_message_ids, friend_id);

	struct MessagesPacket parsed_packet;
	if (!parse_messages_packet(data, size, &parsed_packet)) {
		/* FIXME: We should probably tell the sender that they gave us invalid data here */
		clear_incoming_message(incoming_message);
		return;
	}

	if (parsed_packet.message_type == MESSAGE_RECEIPT) {
		ext_message_ids->receipt_cb(friend_id, parsed_packet.receipt_num, ext_message_ids->userdata);
		return;
	}

	if (parsed_packet.message_type == MESSAGE_START) {
		/*
		 * realloc here instead of malloc because we may have dropped half a message
		 * if a user went offline half way through sending
		 */
		uint8_t* resized_message = realloc(incoming_message->message, parsed_packet.total_message_size);
		if (!resized_message)  {
			/* FIXME: We should probably tell the sender that we dropped a message here */
			clear_incoming_message(incoming_message);
			return;
		}

		incoming_message->message = resized_message;
		incoming_message->size = 0;
		incoming_message->capacity = parsed_packet.total_message_size;
		incoming_message->receipt_num = parsed_packet.receipt_num;
	}

	if (parsed_packet.message_type == MESSAGE_FINISH && incoming_message->size == 0) {
		/* We can skip the allocate/memcpy here */
		if (ext_message_ids->cb) {
			ext_message_ids->cb(friend_id, parsed_packet.message_data, parsed_packet.message_size, ext_message_ids->userdata);
		}
		return;
	}


	if (parsed_packet.message_size + incoming_message->size > incoming_message->capacity) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		clear_incoming_message(incoming_message);
		return;
	}

	memcpy(incoming_message->message + incoming_message->size, parsed_packet.message_data, parsed_packet.message_size);
	incoming_message->size += parsed_packet.message_size;

	if (parsed_packet.message_type == MESSAGE_FINISH) {

		if (ext_message_ids->cb) {
			ext_message_ids->cb(friend_id, incoming_message->message, incoming_message->size, ext_message_ids->userdata);
		}
		uint32_t received_receipt_id = incoming_message->receipt_num;
		uint8_t receipt_data[5];
		receipt_data[0] = MESSAGE_RECEIPT;
		receipt_data[1] = (received_receipt_id >> 24) & 0xff;
		receipt_data[2] = (received_receipt_id >> 16) & 0xff;
		receipt_data[3] = (received_receipt_id >> 8) & 0xff;
		receipt_data[4] = (received_receipt_id) & 0xff;

		toxext_packet_append(response_packet, extension, receipt_data, sizeof(receipt_data));
		clear_incoming_message(incoming_message);
	}
}

static void tox_extension_messages_neg(struct ToxExtExtension* extension, uint32_t friend_id, bool compatible, void* userdata, struct ToxExtPacketList* response_packet) {
	(void)extension;
	(void)response_packet;
	struct ToxExtensionMessages *ext_message_ids = userdata;
	get_or_insert_incoming_message(ext_message_ids, friend_id);
	ext_message_ids->negotiated_cb(friend_id, compatible, ext_message_ids->userdata);
}

struct ToxExtensionMessages* tox_extension_messages_register(
    struct ToxExt* toxext,
    tox_extension_messages_received_cb cb,
    tox_extension_messages_receipt_cb receipt_cb,
    tox_extension_messages_negotiate_cb neg_cb,
    void* userdata) {

	assert(cb);

	struct ToxExtensionMessages* extension = malloc(sizeof(struct ToxExtensionMessages));
	extension->extension_handle = toxext_register(toxext, uuid, extension, tox_extension_messages_recv, tox_extension_messages_neg);
	extension->incoming_messages = NULL;
	extension->incoming_messages_size = 0;
	extension->cb = cb;
	extension->receipt_cb = receipt_cb;
	extension->negotiated_cb = neg_cb;
	extension->userdata = userdata;

	if (!extension->extension_handle) {
		free(extension);
		return NULL;
	}

	return extension;
}

void tox_extension_messages_free(struct ToxExtensionMessages* extension) {
	for (size_t i = 0; i < extension->incoming_messages_size; ++i) {
		free(extension->incoming_messages[i].message);
	}
	free(extension->incoming_messages);
	free(extension);
}

void tox_extension_messages_negotiate(struct ToxExtensionMessages* extension, uint32_t friend_id) {
	toxext_negotiate_connection(extension->extension_handle, friend_id);
}

static uint8_t const* tox_extension_messages_chunk(
	bool first_chunk,
	uint8_t const* data,
	size_t size,
	uint8_t* extension_data,
	size_t* output_size) {

	uint8_t const* ret;
	bool bLastChunk = size <= TOXEXT_MAX_PACKET_SIZE - 1;

	if (bLastChunk) {
		extension_data[0] = MESSAGE_FINISH;
		size_t advance_size = size;
		*output_size = size + 1;
		ret = data + advance_size;
		memcpy(extension_data + 1, data, advance_size);
	}
	else if (first_chunk) {
		extension_data[0] = MESSAGE_START;
		extension_data[1] = (size >> 56) & 0xff;
		extension_data[2] = (size >> 48) & 0xff;
		extension_data[3] = (size >> 40) & 0xff;
		extension_data[4] = (size >> 32) & 0xff;
		extension_data[5] = (size >> 24) & 0xff;
		extension_data[6] = (size >> 16) & 0xff;
		extension_data[7] = (size >> 8) & 0xff;
		extension_data[8] = (size) & 0xff;

		extension_data[9] = (receipt_num >> 24) & 0xff;
		extension_data[10] = (receipt_num >> 16) & 0xff;
		extension_data[11] = (receipt_num >> 8) & 0xff;
		extension_data[12] = (receipt_num) & 0xff;
		// insert uint64_t here
		size_t advance_size = TOXEXT_MAX_PACKET_SIZE - 13;
		memcpy(extension_data + 13, data, advance_size);
		*output_size = TOXEXT_MAX_PACKET_SIZE;
		ret = data + advance_size;
	}
	else {
		extension_data[0] = MESSAGE_PART;
		size_t advance_size = TOXEXT_MAX_PACKET_SIZE - 1;
		memcpy(extension_data + 1, data, advance_size);
		*output_size = TOXEXT_MAX_PACKET_SIZE;
		ret = data + advance_size;
	}

	return ret;
}

uint32_t tox_extension_messages_append(struct ToxExtensionMessages* extension, struct ToxExtPacketList* packet, uint8_t const* data, size_t size) {
	uint8_t const* end = data + size;
	uint8_t const* next_chunk = data;
	bool first_chunk = true;
	do {
		uint8_t extension_data[TOXEXT_MAX_PACKET_SIZE];
		size_t size_for_chunk;
		next_chunk = tox_extension_messages_chunk(first_chunk, next_chunk, end - next_chunk, extension_data, &size_for_chunk);
		first_chunk = false;

		toxext_packet_append(packet, extension->extension_handle, extension_data, size_for_chunk);
	} while (end > next_chunk);
	return receipt_num++;
}
