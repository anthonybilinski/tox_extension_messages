#include "../tox_extension_messages.c"

static uint8_t* last_received_buffer = NULL;
static size_t last_received_buffer_size = 0;
static void test_cb(uint32_t friend_number, uint8_t const* message, size_t length, void* user_data) {
    (void)friend_number;
    (void)message;
    (void)user_data;

    free(last_received_buffer);
    last_received_buffer = malloc(length);
    last_received_buffer_size = length;
    memcpy(last_received_buffer, message, length);
}

static void test_neg_cb(uint32_t friend_number, bool compatible, void* user_data) {
    (void)friend_number;
    (void)compatible;
    (void)user_data;
}

static struct ToxExtensionMessages* s_messages;


static char const zero_sized_buffer[] = "";
static char const small_sized_buffer[] = "asdf";
static uint8_t med_sized_buffer[TOXEXT_MAX_PACKET_SIZE * 2 -  TOXEXT_MAX_PACKET_SIZE / 2];
static uint8_t large_sized_buffer[TOXEXT_MAX_PACKET_SIZE * 3 -  TOXEXT_MAX_PACKET_SIZE / 2];

static void fill_buffer(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = i % 255;
    }
}

int toxext_packet_append(
	struct ToxExtPacketList* packet/*in/out*/,
	struct ToxExtExtension* extension,
	void const* data,
	size_t size) {

    (void)packet;
    (void)extension;
    (void)data;
    (void)size;
    return 0;
}

struct ToxExtExtension* toxext_register(
	struct ToxExt* toxext,
	uint8_t const* uuid,
	void* userdata,
	toxext_recv_callback recv_cb,
	toxext_negotiate_connection_cb neg_cb) {

    (void)toxext;
    (void)uuid;
    (void)userdata;
    (void)recv_cb;
    (void)neg_cb;
    return NULL;
}

int toxext_negotiate_connection(
	struct ToxExtExtension* extension,
	uint32_t friend_id) {

    (void)extension;
    (void)friend_id;
    return 0;
}

static void test_packet(uint8_t const* data, size_t size) {
	uint8_t const* end = data + size;
	uint8_t const* next_chunk = data;
	bool first_chunk = true;
    do {
		uint8_t extension_data[TOXEXT_MAX_PACKET_SIZE];
		size_t size_for_chunk;
		next_chunk = tox_extension_messages_chunk(first_chunk, next_chunk, end - next_chunk, extension_data, &size_for_chunk);
		first_chunk = false;

		tox_extension_messages_recv(
            NULL,
            0,
            extension_data,
            size_for_chunk,
            s_messages,
            NULL);
    } while (end > next_chunk);
}


/**
 * I couldn't be arsed to write a better test. There are tons of hacks in here.
 * Just trying to ensure the logic of the few different packet cases are handled correctly
 */
int main(void) {
    s_messages = malloc(sizeof(struct ToxExtensionMessages));
    s_messages->extension_handle = NULL;
    s_messages->incoming_messages = NULL;
    s_messages->incoming_messages_size = 0;
    s_messages->cb = test_cb;
    s_messages->negotiated_cb = test_neg_cb;
    s_messages->userdata = NULL;

    tox_extension_messages_neg(NULL, 0, true, s_messages, NULL);

    test_packet((uint8_t*)small_sized_buffer, sizeof(small_sized_buffer));
    assert(last_received_buffer_size == sizeof(small_sized_buffer));
    assert(memcmp(last_received_buffer, small_sized_buffer, last_received_buffer_size) == 0);

    fill_buffer(med_sized_buffer, sizeof(med_sized_buffer));
    test_packet((uint8_t*)med_sized_buffer, sizeof(med_sized_buffer));
    assert(last_received_buffer_size == sizeof(med_sized_buffer));
    assert(memcmp(last_received_buffer, med_sized_buffer, last_received_buffer_size) == 0);

    fill_buffer(large_sized_buffer, sizeof(large_sized_buffer));
    test_packet((uint8_t*)large_sized_buffer, sizeof(large_sized_buffer));
    assert(last_received_buffer_size == sizeof(large_sized_buffer));
    assert(memcmp(last_received_buffer, large_sized_buffer, last_received_buffer_size) == 0);

    test_packet((uint8_t*)zero_sized_buffer, 0);
    assert(last_received_buffer_size == 0);
    assert(memcmp(last_received_buffer, zero_sized_buffer, last_received_buffer_size) == 0);

    tox_extension_messages_free(s_messages);
    free(last_received_buffer);
    return 0;
}
