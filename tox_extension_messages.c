#include "tox_extension_messages.h"

#include <toxext/toxext.h>
#include <toxext/toxext_util.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint8_t const uuid[16] = { 0x9e, 0x10, 0x03, 0x16, 0xd2, 0x6f,
				  0x45, 0x39, 0x8c, 0xdb, 0xae, 0x81,
				  0x00, 0x42, 0xf8, 0x64 };

struct IncomingMessage {
	uint32_t friend_id;
	uint8_t *message;
	size_t size;
	size_t capacity;
};

enum Messages {
	MESSAGE_NEGOTIATE,
	MESSAGE_START,
	MESSAGE_PART,
	MESSAGE_FINISH,
	MESSAGE_RECEIVED,
};

struct FriendMaxSendingSize {
	uint32_t friend_id;
	uint64_t max_sending_message_size;
};

struct FriendSendingInvalidSize {
	uint32_t friend_id;
	bool sending_invalid;
};

struct ToxExtensionMessages {
	struct ToxExtExtension *extension_handle;
	// Ideally we would use a better data structure for this but C doesn't have a ton available
	struct IncomingMessage *incoming_messages;
	size_t incoming_messages_size;
	uint64_t next_receipt_id;
	tox_extension_messages_received_cb cb;
	tox_extension_messages_receipt_cb receipt_cb;
	tox_extension_messages_negotiate_cb negotiated_cb;
	void *userdata;
	size_t num_negotiated;
	struct FriendMaxSendingSize *max_sending_message_sizes;
	struct FriendSendingInvalidSize *ongoing_invalid_messages;
	uint64_t max_receiving_message_size;
};

static struct IncomingMessage *
get_incoming_message(struct ToxExtensionMessages *extension, uint32_t friend_id)
{
	for (size_t i = 0; i < extension->incoming_messages_size; ++i) {
		if (extension->incoming_messages[i].friend_id == friend_id) {
			return &extension->incoming_messages[i];
		}
	}

	return NULL;
}

static void init_incoming_message(struct IncomingMessage *incoming_message,
				  uint32_t friend_id)
{
	incoming_message->friend_id = friend_id;
	incoming_message->message = NULL;
	incoming_message->size = 0;
	incoming_message->capacity = 0;
}

static struct IncomingMessage *
insert_incoming_message(struct ToxExtensionMessages *extension,
			uint32_t friend_id)
{
	struct IncomingMessage *new_incoming_messages =
		realloc(extension->incoming_messages,
			(extension->incoming_messages_size + 1) *
				sizeof(struct IncomingMessage));

	if (!new_incoming_messages) {
		return NULL;
	}

	extension->incoming_messages = new_incoming_messages;
	extension->incoming_messages_size++;

	struct IncomingMessage *incoming_message =
		&extension->incoming_messages[extension->incoming_messages_size -
					      1];
	init_incoming_message(incoming_message, friend_id);
	return incoming_message;
}

static struct IncomingMessage *
get_or_insert_incoming_message(struct ToxExtensionMessages *extension,
			       uint32_t friend_id)
{
	struct IncomingMessage *incoming_message =
		get_incoming_message(extension, friend_id);

	if (!incoming_message) {
		incoming_message =
			insert_incoming_message(extension, friend_id);
	}

	return incoming_message;
}

static void clear_incoming_message(struct IncomingMessage *incoming_message)
{
	free(incoming_message->message);
	incoming_message->message = NULL;
	incoming_message->size = 0;
	incoming_message->capacity = 0;
}

struct MessagesPacket {
	enum Messages message_type;
	/* On start packets we flag how large the entire buffer will be */
	size_t total_message_size;
	uint8_t const *message_data;
	size_t message_size;
	size_t receipt_id;
	uint64_t max_sending_message_size;
};

struct FriendSendingInvalidSize *
get_friend_dropping(struct ToxExtensionMessages *extension, uint32_t friend_id)
{
	for (size_t i = 0; i < extension->num_negotiated; ++i) {
		if (extension->ongoing_invalid_messages[i].friend_id ==
		    friend_id) {
			return &extension->ongoing_invalid_messages[i];
		}
	}
	assert(false);
	return NULL;
}

bool parse_messages_packet(uint8_t const *data, size_t size,
			   struct MessagesPacket *messages_packet)
{
	uint8_t const *it = data;
	uint8_t const *end = data + size;

	if (it + 1 > end) {
		return false;
	}
	messages_packet->message_type = *it;
	it += 1;

	if (messages_packet->message_type == MESSAGE_RECEIVED) {
		messages_packet->receipt_id =
			toxext_read_from_buf(uint64_t, it, 8);
		return true;
	}

	if (messages_packet->message_type == MESSAGE_START) {
		if (it + 8 > end) {
			return false;
		}

		messages_packet->total_message_size =
			toxext_read_from_buf(uint64_t, it, 8);
		it += 8;
	}

	if (messages_packet->message_type == MESSAGE_FINISH) {
		messages_packet->receipt_id =
			toxext_read_from_buf(uint64_t, it, 8);
		it += 8;
	}

	if (messages_packet->message_type == MESSAGE_NEGOTIATE) {
		messages_packet->max_sending_message_size =
			toxext_read_from_buf(uint64_t, it, 8);
		it += 8;
	}

	if (it > end) {
		return false;
	}

	messages_packet->message_data = it;
	messages_packet->message_size = end - it;

	return true;
}

void tox_extension_messages_negotiate_size_cb(
	struct ToxExtensionMessages *extension, uint32_t friend_id,
	uint64_t max_sending_message_size)
{
	struct FriendMaxSendingSize *new_max_sendings_sizes =
		realloc(extension->max_sending_message_sizes,
			(extension->num_negotiated + 1) *
				sizeof(struct FriendMaxSendingSize));
	if (!new_max_sendings_sizes) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		return;
	}

	struct FriendSendingInvalidSize *new_ongoing_invalid_messages =
		realloc(extension->ongoing_invalid_messages,
			(extension->num_negotiated + 1) *
				sizeof(struct FriendSendingInvalidSize));
	if (!new_ongoing_invalid_messages) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		return;
	}

	extension->max_sending_message_sizes = new_max_sendings_sizes;
	extension->ongoing_invalid_messages = new_ongoing_invalid_messages;
	extension->num_negotiated++;

	struct FriendMaxSendingSize *new_max_size =
		&extension->max_sending_message_sizes[extension->num_negotiated -
						      1];
	new_max_size->friend_id = friend_id;
	new_max_size->max_sending_message_size = max_sending_message_size;

	struct FriendSendingInvalidSize *new_sending_invalid =
		&extension->ongoing_invalid_messages[extension->num_negotiated -
						     1];
	new_sending_invalid->friend_id = friend_id;
	new_sending_invalid->sending_invalid = false;

	extension->negotiated_cb(friend_id, true, extension->userdata);
}

void tox_extension_messages_negotiate_size(
	struct ToxExtensionMessages *extension,
	struct ToxExtPacketList *response_packet_list)
{
	uint8_t data[9];
	data[0] = MESSAGE_NEGOTIATE;
	toxext_write_to_buf(extension->max_receiving_message_size, data + 1, 8);
	toxext_segment_append(response_packet_list, extension->extension_handle,
			      data, 9);
	return;
}

void tox_extension_copy_in_message_data(struct MessagesPacket *parsed_packet,
					struct IncomingMessage *incoming_message)
{
	if (parsed_packet->message_size + incoming_message->size >
	    incoming_message->capacity) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		clear_incoming_message(incoming_message);
		return;
	}

	memcpy(incoming_message->message + incoming_message->size,
	       parsed_packet->message_data, parsed_packet->message_size);
	incoming_message->size += parsed_packet->message_size;
}

void tox_extension_messages_handle_message_start(
	struct ToxExtensionMessages *extension, uint32_t friend_id,
	struct MessagesPacket *parsed_packet,
	struct IncomingMessage *incoming_message)
{
	if (extension->max_receiving_message_size <
	    parsed_packet->total_message_size) {
		struct FriendSendingInvalidSize *friend_dropping =
			get_friend_dropping(extension, friend_id);
		friend_dropping->sending_invalid = true;
		return;
	}

	/*
		* realloc here instead of malloc because we may have dropped half a message
		* if a user went offline half way through sending
		*/
	uint8_t *resized_message = realloc(incoming_message->message,
					   parsed_packet->total_message_size);
	if (!resized_message) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		clear_incoming_message(incoming_message);
		return;
	}

	incoming_message->message = resized_message;
	incoming_message->size = 0;
	incoming_message->capacity = parsed_packet->total_message_size;

	tox_extension_copy_in_message_data(parsed_packet, incoming_message);
}

void tox_extension_messages_handle_message_finish(
	struct ToxExtensionMessages *extension, uint32_t friend_id,
	struct MessagesPacket *parsed_packet,
	struct IncomingMessage *incoming_message,
	struct ToxExtPacketList *response_packet_list)
{
	/* We can skip the allocate/memcpy here */
	if (incoming_message->size == 0) {
		struct FriendSendingInvalidSize *friend_dropping =
			get_friend_dropping(extension, friend_id);
		bool end_of_dropped_message = friend_dropping->sending_invalid;
		friend_dropping->sending_invalid = false;

		if (end_of_dropped_message ||
		    extension->max_receiving_message_size <
			    parsed_packet->message_size) {
			/* FIXME: We should probably tell the sender that we dropped a message here */
			clear_incoming_message(incoming_message);
			return;
		}

		if (extension->cb) {
			extension->cb(friend_id, parsed_packet->message_data,
				      parsed_packet->message_size,
				      extension->userdata);
		}

		uint8_t data[9];
		data[0] = MESSAGE_RECEIVED;
		toxext_write_to_buf(parsed_packet->receipt_id, data + 1, 8);
		toxext_segment_append(response_packet_list,
				      extension->extension_handle, data, 9);

		return;
	}

	tox_extension_copy_in_message_data(parsed_packet, incoming_message);

	struct FriendSendingInvalidSize *friend_dropping =
		get_friend_dropping(extension, friend_id);
	bool end_of_dropped_message = friend_dropping->sending_invalid;
	friend_dropping->sending_invalid = false;

	if (end_of_dropped_message ||
	    extension->max_receiving_message_size < incoming_message->size) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		clear_incoming_message(incoming_message);
		return;
	}

	if (extension->cb) {
		extension->cb(friend_id, incoming_message->message,
			      incoming_message->size, extension->userdata);
	}

	uint8_t data[9];
	data[0] = MESSAGE_RECEIVED;
	toxext_write_to_buf(parsed_packet->receipt_id, data + 1, 8);
	toxext_segment_append(response_packet_list, extension->extension_handle,
			      data, 9);

	clear_incoming_message(incoming_message);
}

void tox_extension_messages_handle_message_part(
	struct ToxExtensionMessages *extension, uint32_t friend_id,
	struct MessagesPacket *parsed_packet,
	struct IncomingMessage *incoming_message)
{
	struct FriendSendingInvalidSize *friend_dropping =
		get_friend_dropping(extension, friend_id);
	if (friend_dropping->sending_invalid) {
		/* FIXME: We should probably tell the sender that we dropped a message here */
		clear_incoming_message(incoming_message);
		return;
	}
	tox_extension_copy_in_message_data(parsed_packet, incoming_message);
}

static void
tox_extension_messages_recv(struct ToxExtExtension *extension,
			    uint32_t friend_id, void const *data, size_t size,
			    void *userdata,
			    struct ToxExtPacketList *response_packet_list)
{
	(void)extension;
	struct ToxExtensionMessages *ext_messages = userdata;
	struct IncomingMessage *incoming_message =
		get_incoming_message(ext_messages, friend_id);

	struct MessagesPacket parsed_packet;
	if (!parse_messages_packet(data, size, &parsed_packet)) {
		/* FIXME: We should probably tell the sender that they gave us invalid data here */
		clear_incoming_message(incoming_message);
		return;
	}

	switch (parsed_packet.message_type) {
	case MESSAGE_NEGOTIATE:
		tox_extension_messages_negotiate_size_cb(
			ext_messages, friend_id,
			parsed_packet.max_sending_message_size);
		return;
	case MESSAGE_START:
		tox_extension_messages_handle_message_start(ext_messages,
							    friend_id,
							    &parsed_packet,
							    incoming_message);
		return;
	case MESSAGE_PART: {
		tox_extension_messages_handle_message_part(ext_messages,
							   friend_id,
							   &parsed_packet,
							   incoming_message);
		return;
	}
	case MESSAGE_FINISH:
		tox_extension_messages_handle_message_finish(
			ext_messages, friend_id, &parsed_packet,
			incoming_message, response_packet_list);
		return;
	case MESSAGE_RECEIVED:
		ext_messages->receipt_cb(friend_id, parsed_packet.receipt_id,
					 ext_messages->userdata);
		return;
	}
}

static void
tox_extension_messages_neg(struct ToxExtExtension *extension,
			   uint32_t friend_id, bool compatible, void *userdata,
			   struct ToxExtPacketList *response_packet_list)
{
	(void)extension;
	struct ToxExtensionMessages *ext_messages = userdata;
	get_or_insert_incoming_message(ext_messages, friend_id);
	if (!compatible) {
		ext_messages->negotiated_cb(friend_id, compatible,
					    ext_messages->userdata);
	} else {
		tox_extension_messages_negotiate_size(ext_messages,
						      response_packet_list);
	}
}

struct ToxExtensionMessages *
tox_extension_messages_register(struct ToxExt *toxext,
				tox_extension_messages_received_cb cb,
				tox_extension_messages_receipt_cb receipt_cb,
				tox_extension_messages_negotiate_cb neg_cb,
				void *userdata, uint64_t max_receive_size)
{
	assert(cb);

	struct ToxExtensionMessages *extension =
		malloc(sizeof(struct ToxExtensionMessages));

	if (!extension) {
		return NULL;
	}

	extension->extension_handle =
		toxext_register(toxext, uuid, extension,
				tox_extension_messages_recv,
				tox_extension_messages_neg);
	extension->incoming_messages = NULL;
	extension->incoming_messages_size = 0;
	extension->next_receipt_id = 0;
	extension->cb = cb;
	extension->receipt_cb = receipt_cb;
	extension->negotiated_cb = neg_cb;
	extension->userdata = userdata;
	extension->max_sending_message_sizes = NULL;
	extension->max_receiving_message_size = max_receive_size;
	extension->ongoing_invalid_messages = NULL;

	if (!extension->extension_handle) {
		free(extension);
		return NULL;
	}

	return extension;
}

void tox_extension_messages_free(struct ToxExtensionMessages *extension)
{
	for (size_t i = 0; i < extension->incoming_messages_size; ++i) {
		free(extension->incoming_messages[i].message);
	}
	free(extension->incoming_messages);
	free(extension->max_sending_message_sizes);
	free(extension->ongoing_invalid_messages);
	free(extension);
}

void tox_extension_messages_negotiate(struct ToxExtensionMessages *extension,
				      uint32_t friend_id)
{
	toxext_negotiate_connection(extension->extension_handle, friend_id);
}

static uint8_t const *
tox_extension_messages_chunk(bool first_chunk, uint8_t const *data, size_t size,
			     uint64_t receipt_id, uint8_t *extension_data,
			     size_t *output_size)
{
	uint8_t const *ret;
	bool last_chunk = size <= TOXEXT_MAX_SEGMENT_SIZE - 9;

	if (last_chunk) {
		extension_data[0] = MESSAGE_FINISH;
		toxext_write_to_buf(receipt_id, extension_data + 1, 8);

		size_t advance_size = size;
		*output_size = size + 9;
		ret = data + advance_size;
		memcpy(extension_data + 9, data, advance_size);
	} else if (first_chunk) {
		extension_data[0] = MESSAGE_START;
		toxext_write_to_buf(size, extension_data + 1, 8);
		size_t advance_size = TOXEXT_MAX_SEGMENT_SIZE - 9;
		memcpy(extension_data + 9, data, advance_size);
		*output_size = TOXEXT_MAX_SEGMENT_SIZE;
		ret = data + advance_size;
	} else {
		extension_data[0] = MESSAGE_PART;
		size_t advance_size = TOXEXT_MAX_SEGMENT_SIZE - 1;
		memcpy(extension_data + 1, data, advance_size);
		*output_size = TOXEXT_MAX_SEGMENT_SIZE;
		ret = data + advance_size;
	}

	return ret;
}

uint64_t tox_extension_messages_append(struct ToxExtensionMessages *extension,
				       struct ToxExtPacketList *packet_list,
				       uint8_t const *data, size_t size,
				       uint32_t friend_id,
				       enum Tox_Extension_Messages_Error *err)
{
	enum Tox_Extension_Messages_Error get_max_err;
	uint64_t max_sending_size = tox_extension_messages_get_max_sending_size(
		extension, friend_id, &get_max_err);
	if (get_max_err != TOX_EXTENSION_MESSAGES_SUCCESS ||
	    size > max_sending_size) {
		if (err) {
			*err = TOX_EXTENSION_MESSAGES_INVALID_ARG;
		}
		return -1;
	}

	uint8_t const *end = data + size;
	uint8_t const *next_chunk = data;
	bool first_chunk = true;
	uint64_t receipt_id = extension->next_receipt_id++;
	do {
		uint8_t extension_data[TOXEXT_MAX_SEGMENT_SIZE];
		size_t size_for_chunk;
		next_chunk = tox_extension_messages_chunk(
			first_chunk, next_chunk, end - next_chunk, receipt_id,
			extension_data, &size_for_chunk);
		first_chunk = false;

		toxext_segment_append(packet_list, extension->extension_handle,
				      extension_data, size_for_chunk);
	} while (end > next_chunk);

	if (err) {
		*err = TOX_EXTENSION_MESSAGES_SUCCESS;
	}
	return receipt_id;
}

uint64_t tox_extension_messages_get_max_receiving_size(
	struct ToxExtensionMessages *extension)
{
	return extension->max_receiving_message_size;
}

uint64_t tox_extension_messages_get_max_sending_size(
	struct ToxExtensionMessages *extension, uint32_t friend_id,
	enum Tox_Extension_Messages_Error *err)
{
	for (size_t i = 0; i < extension->num_negotiated; ++i) {
		if (extension->max_sending_message_sizes[i].friend_id ==
		    friend_id) {
			if (err) {
				*err = TOX_EXTENSION_MESSAGES_SUCCESS;
			}
			return extension->max_sending_message_sizes[i]
				.max_sending_message_size;
		}
	}

	if (err) {
		*err = TOX_EXTENSION_MESSAGES_INVALID_ARG;
	}
	return 0;
}
