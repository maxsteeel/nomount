#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <sys/system_properties.h>

#define kLongLegacyErrorBufferSize 56

typedef struct prop_info {
 atomic_uint_least32_t serial;
 union {
   char value[PROP_VALUE_MAX];
   struct {
     char error_message[kLongLegacyErrorBufferSize];
     uint32_t offset;
   } long_property;
 };
 char name[0];
} prop_info;

_Static_assert(sizeof(prop_info) == 96, "sizeof struct prop_info must be 96 bytes");
