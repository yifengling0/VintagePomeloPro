#pragma once
#include <stdbool.h>
#include <stdint.h>
/* SDL3 public virtual joystick ABI, introduced in 3.2.0. This declaration
 * follows SDL_joystick.h, revision ab5541ec5 (the installed official SDL ELF).
 * Interface version is sizeof(desc), as required by SDL_INIT_INTERFACE.
 * https://github.com/libsdl-org/SDL/blob/ab5541ec5/include/SDL3/SDL_joystick.h */
typedef struct {
    uint32_t version;
    uint16_t type,padding,vendor_id,product_id,naxes,nbuttons,nballs,nhats,ntouchpads,nsensors,padding2[2];
    uint32_t button_mask,axis_mask;
    const char *name;
    const void *touchpads,*sensors;
    void *userdata;
    void (*Update)(void*);
    void (*SetPlayerIndex)(void*,int);
    bool (*Rumble)(void*,uint16_t,uint16_t);
    bool (*RumbleTriggers)(void*,uint16_t,uint16_t);
    bool (*SetLED)(void*,uint8_t,uint8_t,uint8_t);
    bool (*SendEffect)(void*,const void*,int);
    bool (*SetSensorsEnabled)(void*,bool);
    void (*Cleanup)(void*);
} HmosVirtualJoystickDesc;
_Static_assert(sizeof(HmosVirtualJoystickDesc)==136,"Unexpected SDL3 ARM64 virtual joystick ABI");
