#pragma once

/* Input key declarations only; host tests do not use ADC, PWM, or GPIO. */
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack,
    InputKeyMAX,
} InputKey;
