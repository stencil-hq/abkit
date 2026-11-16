// Headless ABHost Mach presentation receiver. Prints JSON lines; never opens a window.
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct Buffer {
    uint32_t block;
    uint32_t identifier;
    IOSurfaceRef surface;
    struct Buffer* next;
};

static struct Buffer* buffers;

static struct Buffer** findBuffer(uint32_t block, uint32_t identifier)
{
    struct Buffer** slot = &buffers;
    while (*slot && ((*slot)->block != block || (*slot)->identifier != identifier))
        slot = &(*slot)->next;
    return slot;
}

static void removeBuffers(uint32_t block, uint32_t identifier, int all)
{
    struct Buffer** slot = &buffers;
    while (*slot) {
        struct Buffer* buffer = *slot;
        if (buffer->block != block || (!all && buffer->identifier != identifier)) {
            slot = &buffer->next;
            continue;
        }
        *slot = buffer->next;
        CFRelease(buffer->surface);
        free(buffer);
    }
}

static int fail(const char* message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return fail("usage: present MACH_SERVICE");
    mach_port_t service = MACH_PORT_NULL;
    mach_port_t host = MACH_PORT_NULL;
    if (bootstrap_check_in(bootstrap_port, argv[1], &service) != KERN_SUCCESS)
        return fail("bootstrap_check_in failed");
    mach_port_limits_t limits = { MACH_PORT_QLIMIT_LARGE };
    if (mach_port_set_attributes(mach_task_self(), service, MACH_PORT_LIMITS_INFO,
        (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT) != KERN_SUCCESS)
        return fail("setting the service queue limit failed");
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("{\"ready\":true}");
    for (;;) {
        union {
            mach_msg_header_t header;
            uint32_t words[128];
        } message;
        kern_return_t result = mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof message,
            service, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (result != KERN_SUCCESS)
            return fail("mach_msg receive failed");
        uint32_t* words = message.words;
        switch (message.header.msgh_id) {
        case 1:
            if (message.header.msgh_size != 48 || words[10] != 1 || host)
                return fail("invalid hello");
            host = words[7];
            puts("{\"hello\":true}");
            break;
        case 2: {
            if (message.header.msgh_size != 56 || !host)
                return fail("invalid buffer message");
            uint32_t block = words[10], identifier = words[11];
            if (*findBuffer(block, identifier))
                return fail("duplicate buffer identifier");
            IOSurfaceRef surface = IOSurfaceLookupFromMachPort(words[7]);
            mach_port_deallocate(mach_task_self(), words[7]);
            if (!surface)
                return fail("IOSurfaceLookupFromMachPort failed");
            if (IOSurfaceGetWidth(surface) != words[12] || IOSurfaceGetHeight(surface) != words[13]
                || IOSurfaceGetBytesPerElement(surface) != 4)
                return fail("IOSurface dimensions or pixel format disagree with buffer message");
            struct Buffer* buffer = malloc(sizeof *buffer);
            if (!buffer)
                return fail("allocating buffer failed");
            *buffer = (struct Buffer) { block, identifier, surface, buffers };
            buffers = buffer;
            break;
        }
        case 3: {
            if (message.header.msgh_size != 32 || !host)
                return fail("invalid frame message");
            uint32_t block = words[6], identifier = words[7];
            struct Buffer* buffer = *findBuffer(block, identifier);
            if (!buffer)
                return fail("frame arrived before its buffer");
            IOSurfaceRef surface = buffer->surface;
            if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != KERN_SUCCESS)
                return fail("IOSurfaceLock failed");
            const uint8_t* pixels = IOSurfaceGetBaseAddress(surface);
            size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
            size_t stride = IOSurfaceGetBytesPerRow(surface), count = 0;
            double sum = 0, squares = 0;
            // Sample luminance across the frame, not alpha (a flat transparent surface is not UI).
            for (size_t y = 0; y < height; y += 8) {
                for (size_t x = 0; x < width; x += 8) {
                    const uint8_t* pixel = pixels + y * stride + x * 4;
                    double luminance = (pixel[0] + pixel[1] + pixel[2]) / 3.0;
                    sum += luminance;
                    squares += luminance * luminance;
                    ++count;
                }
            }
            double variance = count ? squares / count - (sum / count) * (sum / count) : 0;
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            printf("{\"block\":%u,\"buffer\":%u,\"width\":%zu,\"height\":%zu,\"variance\":%.3f}\n",
                block, identifier, width, height, variance);
            // Finished reading: the host may reuse this surface immediately.
            struct { mach_msg_header_t header; uint32_t words[2]; } released = { 0 };
            released.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
            released.header.msgh_size = sizeof released;
            released.header.msgh_remote_port = host;
            released.header.msgh_id = 16;
            released.words[0] = block;
            released.words[1] = identifier;
            if (mach_msg(&released.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof released,
                0, MACH_PORT_NULL, 1000, MACH_PORT_NULL) != KERN_SUCCESS)
                return fail("sending released failed");
            break;
        }
        case 4:
            if (message.header.msgh_size != 32)
                return fail("invalid gone message");
            removeBuffers(words[6], words[7], 0);
            break;
        case 5:
            if (message.header.msgh_size != 28)
                return fail("invalid bye message");
            removeBuffers(words[6], 0, 1);
            break;
        default:
            return fail("unknown Mach message");
        }
    }
}
