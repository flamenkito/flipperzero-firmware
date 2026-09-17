#include <assert.h>
#include <string.h>
#include "../../applications_user/pocket_airbridge/airbridge_passwords.h"
#include "../../applications_user/pocket_airbridge/airbridge_mouse.h"

int main(void) {
    AirbridgePasswords passwords;
    const char* valid = "\r\nwork=  a=b  \r\npersonal=other\n";
    assert(airbridge_passwords_parse(&passwords, valid, strlen(valid)));
    assert(passwords.count == 2);
    assert(strcmp(passwords.entries[0].name, "work") == 0);
    assert(strcmp(passwords.entries[0].value, "  a=b  ") == 0);
    assert(strcmp(passwords.entries[1].value, "other") == 0);
    const char* invalid[] = {
        "missing", "=value", "name=", "n=a\nn=b", "a=x\rvalue", "a=tab\t", "a=\x80", "a=ok\nbad"};
    for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!airbridge_passwords_parse(&passwords, invalid[i], strlen(invalid[i])));
        assert(passwords.count == 0 && passwords.entries[0].value[0] == 0);
    }
    assert(!airbridge_passwords_parse(&passwords, "a=x\0y", 5));
    char boundary[2 + AIRBRIDGE_PASSWORD_SIZE + 2];
    memcpy(boundary, "a=", 2);
    memset(boundary + 2, 'x', sizeof(boundary) - 2);
    assert(airbridge_passwords_parse(&passwords, boundary, 2 + AIRBRIDGE_PASSWORD_SIZE));
    assert(strlen(passwords.entries[0].value) == AIRBRIDGE_PASSWORD_SIZE);
    assert(!airbridge_passwords_parse(&passwords, boundary, 3 + AIRBRIDGE_PASSWORD_SIZE));
    assert(!airbridge_passwords_parse(&passwords, "", AIRBRIDGE_PASSWORD_FILE_MAX + 1));
    assert(airbridge_passwords_parse(&passwords, "", 0) && passwords.count == 0);
    AirbridgeMouse mouse = {0};
    assert(!airbridge_mouse_due(&mouse, false, 0));
    assert(!airbridge_mouse_due(&mouse, true, 10));
    assert(!airbridge_mouse_due(&mouse, true, 60009));
    assert(airbridge_mouse_due(&mouse, true, 60010));
    assert(!airbridge_mouse_due(&mouse, true, 60010));
    assert(!airbridge_mouse_due(&mouse, false, 70000));
    assert(!airbridge_mouse_due(&mouse, true, UINT32_MAX - 100));
    assert(airbridge_mouse_due(&mouse, true, 59899));
    assert(!airbridge_mouse_due(&mouse, true, 59900));
    return 0;
}
