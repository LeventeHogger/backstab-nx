// 32 bites (AArch32) Switch-próba: framebuffer-kitöltés + napló, '+' gombra kilép.
#include <string.h>
#include <stdio.h>
#include <switch.h>

int main(int argc, char **argv) {
    char msg[128];
    snprintf(msg, sizeof(msg), "backstab-nx hello: sizeof(void*)=%u", (unsigned)sizeof(void *));
    svcOutputDebugString(msg, strlen(msg));

    Framebuffer fb;
    framebufferCreate(&fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    u32 frame = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;

        u32 stride;
        u32 *px = (u32 *)framebufferBegin(&fb, &stride);
        for (u32 y = 0; y < 720; y++)
            for (u32 x = 0; x < 1280; x++)
                px[y * stride / 4 + x] = 0xFF000000 | ((x + frame) & 0xFF) << 8 | (y & 0xFF) << 16 | 0x40;
        framebufferEnd(&fb);
        frame += 4;
    }
    framebufferClose(&fb);
    return 0;
}
