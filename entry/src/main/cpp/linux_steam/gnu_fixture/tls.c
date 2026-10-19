#include <stdint.h>
static _Thread_local uint32_t value = 17;
uint32_t steamohos_tls_exchange(uint32_t next) {
    uint32_t old = value; value = next; return old;
}
