/*
 * LD_PRELOAD shim for mvsink on webOS 2.x
 *
 * Intercept all GstreamerProcess flag setters/getters to prevent the
 * msil_scaler functions from entering "DO NOTHING" mode. The real
 * msil_scaler functions in libmsil_lm15u.so will execute normally
 * once these flags return 0.
 *
 * Compile: arm-linux-gnueabi-gcc -shared -fPIC -nostdlib gst_preload.c -o gst_preload.so
 */
#include <stdio.h>

typedef unsigned char MS_BOOL;
typedef unsigned char MS_U8;

/* Block all GstreamerProcess flag setters — always return "not GStreamer" */
void MApi_XC_Set_GstreamerProcess(MS_BOOL bEnable) { (void)bEnable; }
MS_BOOL MApi_XC_IsGstreamerProcess(void) { return 0; }

MS_BOOL MApi_PNL_Set_GstreamerProcess(MS_BOOL bEnable) { (void)bEnable; return 1; }
MS_BOOL MApi_PNL_Get_IsGstreamerProcess(void) { return 0; }

MS_BOOL MApi_PNL_Set_GstreamerProcess_U2(MS_BOOL bEnable) { (void)bEnable; return 1; }
MS_BOOL MApi_PNL_Get_IsGstreamerProcess_U2(void) { return 0; }

/* Block the internal gstreamer_play flag in msil_scaler */
void msil_scaler_set_gstreamer_play(MS_BOOL bEnable) { (void)bEnable; }
