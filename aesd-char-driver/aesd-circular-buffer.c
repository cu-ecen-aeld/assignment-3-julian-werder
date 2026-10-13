/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    uint8_t index;
    uint8_t count;
    uint8_t valid_entries;

    if (buffer == NULL || entry_offset_byte_rtn == NULL) {
        return NULL;
    }

    /* A full buffer has in_offs == out_offs, so the count must be derived from the full flag */
    if (buffer->full) {
        valid_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        valid_entries = (uint8_t)((buffer->in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs)
                                  % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    }

    index = buffer->out_offs;
    for (count = 0; count < valid_entries; count++) {
        struct aesd_buffer_entry *entry = &buffer->entry[index];

        /* Subtract instead of keeping a running sum so the offset can never overflow */
        if (char_offset < entry->size) {
            *entry_offset_byte_rtn = char_offset;
            return entry;
        }
        char_offset -= entry->size;
        index = (uint8_t)((index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    }

    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    if (buffer == NULL || add_entry == NULL) {
        return;
    }

    /* Copy by value: the caller's struct may live on its stack */
    buffer->entry[buffer->in_offs] = *add_entry;

    /* A write into a full buffer just replaced the oldest entry, so the oldest is now the next one */
    if (buffer->full) {
        buffer->out_offs = (uint8_t)((buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    }
    buffer->in_offs = (uint8_t)((buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    buffer->full = (buffer->in_offs == buffer->out_offs);
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
