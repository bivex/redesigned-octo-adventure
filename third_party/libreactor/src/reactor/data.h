#ifndef REACTOR_DATA_H_INCLUDED
#define REACTOR_DATA_H_INCLUDED

#include <stdlib.h>
#include <stdint.h>

typedef struct data data;

struct data
{
  const void *base;
  size_t      size;
};

data        data_alloc(size_t);
void        data_free(data);
data        data_construct(const char *, size_t);
data        data_select(data, size_t, size_t);
data        data_null(void);
data        data_string(const char *);
const void *data_base(data);
size_t      data_size(data);
int         data_empty(data);
int         data_equal(data, data);
int         data_prefix(data, data);
size_t      data_offset(data, data);
data        data_consume(data, size_t);

/* segment aliases for compatibility */
typedef data segment;
segment     segment_empty(void);
segment     segment_data(void *, size_t);
segment     segment_string(char *);
segment     segment_offset(segment, size_t);
int         segment_equal(segment, segment);
int         segment_equal_case(segment, segment);

#endif /* REACTOR_DATA_H_INCLUDED */
