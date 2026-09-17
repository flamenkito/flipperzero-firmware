#include "airbridge_passwords.h"

#include <string.h>

void airbridge_passwords_clear(AirbridgePasswords* passwords) {
    volatile uint8_t* bytes = (volatile uint8_t*)passwords;
    for(size_t i = 0; i < sizeof(*passwords); i++)
        bytes[i] = 0;
}

bool airbridge_passwords_parse(AirbridgePasswords* passwords, const char* data, size_t size) {
    airbridge_passwords_clear(passwords);
    if(size > AIRBRIDGE_PASSWORD_FILE_MAX || (!data && size)) return false;
    size_t start = 0;
    while(start < size) {
        size_t end = start;
        while(end < size && data[end] != '\n')
            end++;
        size_t length = end - start;
        if(length && data[start + length - 1] == '\r') length--;
        if(length) {
            const char* line = data + start;
            const char* equals = memchr(line, '=', length);
            if(!equals) goto invalid;
            size_t name_size = equals - line;
            size_t value_size = length - name_size - 1;
            if(!name_size || name_size > AIRBRIDGE_PASSWORD_NAME || !value_size ||
               value_size > AIRBRIDGE_PASSWORD_SIZE ||
               passwords->count == AIRBRIDGE_PASSWORD_COUNT)
                goto invalid;
            for(size_t i = 0; i < length; i++)
                if((uint8_t)line[i] < 0x20 || (uint8_t)line[i] > 0x7e) goto invalid;
            for(size_t i = 0; i < passwords->count; i++)
                if(strlen(passwords->entries[i].name) == name_size &&
                   memcmp(passwords->entries[i].name, line, name_size) == 0)
                    goto invalid;
            AirbridgePassword* entry = &passwords->entries[passwords->count++];
            memcpy(entry->name, line, name_size);
            memcpy(entry->value, equals + 1, value_size);
        }
        start = end + 1;
    }
    return true;
invalid:
    airbridge_passwords_clear(passwords);
    return false;
}
