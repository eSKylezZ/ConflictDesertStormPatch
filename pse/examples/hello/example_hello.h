/* example.hello - the interface this plugin publishes for other plugins (pse.messaging RegisterInterface).
 * Another plugin includes this header and asks for it in PSE_MSG_POST_LOAD:
 *     const ExampleHelloV1* hello = (const ExampleHelloV1*)msg->QueryInterface(self, EXAMPLE_HELLO_INTERFACE, 1);
 * NULL = example.hello isn't installed (or is older than version 1). */
#ifndef EXAMPLE_HELLO_H
#define EXAMPLE_HELLO_H

#include <pse/pse_api.h>

#define EXAMPLE_HELLO_INTERFACE "example.hello"

typedef struct ExampleHelloV1 {
    uint32_t size;                         /* sizeof(ExampleHelloV1) - later versions only grow */
    int(PSE_CALL* Add)(int a, int b);
} ExampleHelloV1;

#endif
