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
    if (buffer == NULL || entry_offset_byte_rtn == NULL) {
        return NULL;
    }

    // If the buffer is completely empty, there is nothing to search
    if (buffer->in_offs == buffer->out_offs && !buffer->full) {
        return NULL;
    }

    uint8_t index = buffer->out_offs;
    bool checked_all = false;

    while (!checked_all) {
        struct aesd_buffer_entry *current_entry = &(buffer->entry[index]);

        // If the remaining char_offset falls within this specific entry
        if (char_offset < current_entry->size) {
            *entry_offset_byte_rtn = char_offset;
            return current_entry;
        }

        // Otherwise, subtract this entry's size from our target offset and move to the next entry
        char_offset -= current_entry->size;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        // If we have wrapped around and hit the in_offs pointer, we have checked all valid data
        if (index == buffer->in_offs) {
            checked_all = true;
        }
    }

    // Offset is greater than the total number of characters currently in the buffer
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

    // Write the new entry into the current input offset position
    buffer->entry[buffer->in_offs] = *add_entry;

    // Advance the input offset pointer, wrapping around if necessary
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // If the buffer was already full, the output offset must advance to overwrite the oldest data
    if (buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    // Check if the buffer is now full (input pointer has caught up to the output pointer)
    if (buffer->in_offs == buffer->out_offs) {
        buffer->full = true;
    }
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
