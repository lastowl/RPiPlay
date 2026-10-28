/*
 * Make the GOP (Graphics Output Processor) layers transparent
 * so the video rendered by mvsink on the MVOP layer shows through.
 *
 * Uses dlopen to call MStar GOP API from libkadaptor.so at runtime.
 */
#include <stdio.h>
#include <dlfcn.h>

int main(int argc, char *argv[]) {
    void *lib = dlopen("libkadaptor.so", RTLD_LAZY);
    if (!lib) {
        lib = dlopen("libkadaptor.so.1", RTLD_LAZY);
    }
    if (!lib) {
        lib = dlopen("/usr/lib/libkadaptor.so.1.0.0", RTLD_LAZY);
    }
    if (!lib) {
        fprintf(stderr, "Failed to load libkadaptor.so: %s\n", dlerror());
        return 1;
    }
    printf("Loaded libkadaptor.so\n");

    /* Get function pointers */
    /* MS_U8 MApi_GOP_GWIN_GetTotalGwinNum(void) */
    typedef unsigned char (*fn_GetTotalGwinNum)(void);
    /* MS_U8 MApi_GOP_GWIN_GetMaxGOPNum(void) */
    typedef unsigned char (*fn_GetMaxGOPNum)(void);
    /* MS_BOOL MApi_GOP_GWIN_IsGWINEnabled(MS_U8 u8win) */
    typedef unsigned char (*fn_IsGWINEnabled)(unsigned char);
    /* MS_BOOL MApi_GOP_GWIN_Enable(MS_U8 u8win, MS_BOOL bEnable) */
    typedef unsigned char (*fn_Enable)(unsigned char, unsigned char);
    /* void MApi_GOP_GWIN_SetBlending(MS_U8 u8win, MS_BOOL bEnable, MS_U8 u8coef) */
    typedef void (*fn_SetBlending)(unsigned char, unsigned char, unsigned char);
    /* void MApi_GOP_GWIN_SetAlphaValue(MS_U32 u32val) - may not have this sig */
    typedef void (*fn_SetAlphaValue)(unsigned int);
    /* void MApi_GOP_GWIN_SwitchGOP(MS_U8 u8GOP) */
    typedef void (*fn_SwitchGOP)(unsigned char);
    /* MS_U8 MApi_GOP_GWIN_GetCurrentGOP(void) */
    typedef unsigned char (*fn_GetCurrentGOP)(void);
    /* void gfx_SetFBVisibility(int surfaceId, int visible) - guess */
    typedef void (*fn_SetFBVisibility)(int, int);

    fn_GetTotalGwinNum pGetTotal = (fn_GetTotalGwinNum)dlsym(lib, "MApi_GOP_GWIN_GetTotalGwinNum");
    fn_GetMaxGOPNum pGetMaxGOP = (fn_GetMaxGOPNum)dlsym(lib, "MApi_GOP_GWIN_GetMaxGOPNum");
    fn_IsGWINEnabled pIsEnabled = (fn_IsGWINEnabled)dlsym(lib, "MApi_GOP_GWIN_IsGWINEnabled");
    fn_Enable pEnable = (fn_Enable)dlsym(lib, "MApi_GOP_GWIN_Enable");
    fn_SetBlending pSetBlending = (fn_SetBlending)dlsym(lib, "MApi_GOP_GWIN_SetBlending");
    fn_SwitchGOP pSwitchGOP = (fn_SwitchGOP)dlsym(lib, "MApi_GOP_GWIN_SwitchGOP");
    fn_GetCurrentGOP pGetCurrentGOP = (fn_GetCurrentGOP)dlsym(lib, "MApi_GOP_GWIN_GetCurrentGOP");
    fn_SetFBVisibility pSetFBVis = (fn_SetFBVisibility)dlsym(lib, "gfx_SetFBVisibility");

    printf("GetTotalGwinNum: %p\n", (void*)pGetTotal);
    printf("GetMaxGOPNum: %p\n", (void*)pGetMaxGOP);
    printf("IsGWINEnabled: %p\n", (void*)pIsEnabled);
    printf("Enable: %p\n", (void*)pEnable);
    printf("SetBlending: %p\n", (void*)pSetBlending);
    printf("SwitchGOP: %p\n", (void*)pSwitchGOP);
    printf("GetCurrentGOP: %p\n", (void*)pGetCurrentGOP);
    printf("gfx_SetFBVisibility: %p\n", (void*)pSetFBVis);

    if (argc > 1 && argv[1][0] == 'i') {
        /* Info mode — just report state */
        if (pGetMaxGOP) {
            unsigned char maxgop = pGetMaxGOP();
            printf("Max GOPs: %d\n", maxgop);
        }
        if (pGetTotal) {
            unsigned char total = pGetTotal();
            printf("Total GWINs: %d\n", total);
        }
        if (pGetCurrentGOP) {
            unsigned char cur = pGetCurrentGOP();
            printf("Current GOP: %d\n", cur);
        }
        if (pIsEnabled && pGetTotal) {
            unsigned char total = pGetTotal();
            for (unsigned char i = 0; i < total && i < 32; i++) {
                unsigned char en = pIsEnabled(i);
                printf("  GWIN %d: %s\n", i, en ? "ENABLED" : "disabled");
            }
        }
    } else if (argc > 1 && argv[1][0] == 'd') {
        /* Disable mode — disable all GWINs to make GOP transparent */
        if (pEnable && pGetTotal && pIsEnabled) {
            unsigned char total = pGetTotal();
            printf("Disabling %d GWINs...\n", total);
            for (unsigned char i = 0; i < total && i < 32; i++) {
                if (pIsEnabled(i)) {
                    printf("  Disabling GWIN %d\n", i);
                    pEnable(i, 0);
                }
            }
            printf("Done — UI should be transparent now\n");
        }
    } else if (argc > 1 && argv[1][0] == 'b') {
        /* Blending mode — set all GWINs to transparent blending */
        if (pSetBlending && pGetTotal && pIsEnabled) {
            unsigned char total = pGetTotal();
            printf("Setting %d GWINs to transparent blending...\n", total);
            for (unsigned char i = 0; i < total && i < 32; i++) {
                if (pIsEnabled(i)) {
                    printf("  Setting GWIN %d alpha=0\n", i);
                    pSetBlending(i, 1, 0);  /* enable blending, alpha=0 (transparent) */
                }
            }
            printf("Done\n");
        }
    } else if (argc > 1 && argv[1][0] == 'f') {
        /* FB visibility mode */
        if (pSetFBVis) {
            printf("Setting FB visibility to 0...\n");
            pSetFBVis(0, 0);  /* surface 0, invisible */
            printf("Done\n");
        } else {
            printf("gfx_SetFBVisibility not found\n");
        }
    } else {
        printf("Usage: %s [i|d|b|f]\n", argv[0]);
        printf("  i = info (show GOP/GWIN state)\n");
        printf("  d = disable GWINs (make UI transparent)\n");
        printf("  b = set blending alpha=0 (make UI transparent)\n");
        printf("  f = set FB visibility off\n");
    }

    dlclose(lib);
    return 0;
}
