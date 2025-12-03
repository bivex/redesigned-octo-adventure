#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "buffer.h"
#include "core.h"

static size_t buffer_roundup(size_t size)
{
  /* More aggressive rounding for better performance with HTTP workloads */
  size--;
  size |= size >> 1;
  size |= size >> 2;
  size |= size >> 4;
  size |= size >> 8;
  size |= size >> 16;
  size |= size >> 32;

  /* For sizes under 64KB, round up to next power of 2 */
  if (size < 65536)
    size++;

  /* For larger sizes, use 64KB granularity to avoid excessive memory usage */
  else
    size = (size + 65536) & ~(65536 - 1);

  return size;
}

/* constructor/destructor */

void buffer_construct(buffer *b)
{
  *b = (buffer) {0};
}

void buffer_destruct(buffer *b)
{
  buffer_clear(b);
}

/* capacity */

size_t buffer_size(buffer *b)
{
  return b->size;
}

size_t buffer_capacity(buffer *b)
{
  return b->capacity;
}

void buffer_reserve(buffer *b, size_t capacity)
{
  if (capacity > b->capacity)
  {
    capacity = buffer_roundup(capacity);
    void *new_data = reactor_mem_alloc(capacity);
    if (new_data == NULL)
      return; // Keep original data on failure

    // Copy existing data if any
    if (b->data && b->size > 0) {
      memcpy(new_data, b->data, b->size);
      reactor_mem_free(b->data);
    }

    b->data = new_data;
    b->capacity = capacity;
  }
}

void buffer_resize(buffer *b, size_t size)
{
  if (size > buffer_capacity(b))
    buffer_reserve(b, size);
  b->size = size;
}

void buffer_compact(buffer *b)
{
  if (b->capacity > b->size && b->size > 0)
  {
    void *new_data = reactor_mem_alloc(b->size);
    if (new_data == NULL)
      return; // Keep original data on failure

    memcpy(new_data, b->data, b->size);
    reactor_mem_free(b->data);
    b->data = new_data;
    b->capacity = b->size;
  }
}

/* modifiers */

void buffer_insert(buffer *b, size_t position, void *data, size_t size)
{
  buffer_reserve(b, b->size + size);
  if (position < b->size)
    memmove((char *) b->data + position + size, (char *) b->data + position, b->size - position);
  memcpy((char *) b->data + position, data, size);
  b->size += size;
}

void buffer_insert_fill(buffer *b, size_t position, size_t count, void *data, size_t size)
{
  size_t i;

  buffer_reserve(b, b->size + (count * size));
  if (position < b->size)
    memmove((char *) b->data + position + (count * size), (char *) b->data + position, b->size - position);

  for (i = 0; i < count; i++)
    memcpy((char *) b->data + position + (i * size), data, size);
  b->size += count * size;
}

void buffer_erase(buffer *b, size_t position, size_t size)
{
  /* Optimized: special case for erasing from beginning (most common case) */
  if (position == 0) {
    /* If erasing entire buffer, just reset size */
    if (size >= b->size) {
      b->size = 0;
      return;
    }
    /* Shift remaining data to beginning */
    memmove(b->data, (char *) b->data + size, b->size - size);
  } else {
    /* General case: erase from middle */
    memmove((char *) b->data + position, (char *) b->data + position + size, b->size - position - size);
  }
  b->size -= size;
}

void buffer_clear(buffer *b)
{
  reactor_mem_free(b->data);
  buffer_construct(b);
}

/* element access */

void *buffer_data(buffer *b)
{
  return b->data;
}
