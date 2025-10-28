/* ft600_display_fixed.c
   修正版：增加 stdint.h/limits.h，修正 sign-extend、clamp，消除缩进警告
*/

#include "PainterEngine.h"
#include "platform/modules/px_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "FTD3XX.h"

#include <stdint.h>   // uint16_t, int16_t
#include <limits.h>   // INT16_MAX, INT16_MIN

// Configuration
#define BUFFER_SIZE         (1024 * 1024)
#define PIPE_ID             0x82

// Storage sizes
#define MAX_STORAGE_POINTS  100000
#define CAPTURE_TARGET      10000
#define MAX_DISPLAY_POINTS  1000

// Pre-trigger
#define PRE_TRIGGER_WINDOW  256
#define PRE_TRIGGER_CONFIRM 3

// Globals
FT_HANDLE g_ftHandle = NULL;
volatile px_bool g_running = PX_TRUE;
volatile px_bool g_deviceConnected = PX_FALSE;
px_mutex g_dataMutex;
px_char g_statusMessage[256] = "Initializing...";

volatile px_bool g_littleEndian = PX_TRUE;
volatile px_bool g_raw16bit = PX_FALSE;
volatile px_bool g_high12bit = PX_FALSE;
volatile px_int g_yScale = 4096;
volatile px_int g_sampleRate = 1;
volatile px_bool g_paused = PX_FALSE;

typedef enum { TRIGGER_OFF=0, TRIGGER_RISING, TRIGGER_FALLING, TRIGGER_AUTO } TriggerMode;
volatile TriggerMode g_triggerMode = TRIGGER_RISING;
volatile px_int g_triggerLevel = 0;

typedef struct {
    px_short values[MAX_STORAGE_POINTS];
    px_int writeIndex;
    px_int count;

    px_short preTrigger[PRE_TRIGGER_WINDOW];
    px_int preWrite;
    px_int preCount;

    px_bool triggerPending;
    px_int triggerConfirmCount;
    TriggerMode triggerExpected;

    px_bool capturing;
    px_bool dataReady;
    px_bool drawnOnce;

    px_short prevValues[MAX_STORAGE_POINTS];
    px_int prevCount;

    px_dword totalBytes;
    px_float currentSpeed;
    px_dword lastUpdateTime;
    px_dword lastBytes;

    px_dword readCallCount;
    px_dword successCount;
    px_dword timeoutCount;
    px_dword errorCount;

    px_short minValue;
    px_short maxValue;
    px_float avgValue;

    px_short lastValue;
} DataDisplay;

DataDisplay g_dataDisplay = {0};
volatile px_bool g_threadRunning = PX_FALSE;

/* Robust ParseDataValue:
   - uses int16_t/uint16_t and shift-sign-extend technique
   - clamps to valid ranges for 12-bit or 16-bit
*/
px_short ParseDataValue(UCHAR byte0, UCHAR byte1) {
    uint16_t raw16;

    if (g_littleEndian) {
        raw16 = (uint16_t)(byte0 | (byte1 << 8));
    } else {
        raw16 = (uint16_t)(byte1 | (byte0 << 8));
    }

    if (g_raw16bit) {
        int16_t s = (int16_t)raw16;
        if (s > INT16_MAX) s = INT16_MAX;
        if (s < INT16_MIN) s = INT16_MIN;
        return (px_short)s;
    } else {
        uint16_t value12;
        if (g_high12bit) {
            value12 = (raw16 >> 4) & 0x0FFF;
        } else {
            value12 = raw16 & 0x0FFF;
        }

        // sign-extend 12-bit to 16-bit: shift left then arithmetic shift right
        int16_t s = (int16_t)( (int16_t)(value12 << 4) );
        s >>= 4;

        // explicit clamp to 12-bit signed range
        if (s > 2047) s = 2047;
        if (s < -2048) s = -2048;
        return (px_short)s;
    }
}

/* Initialization */
px_bool InitializeFT600() {
    FT_STATUS status;
    DWORD numDevices = 0;
    PX_strcpy(g_statusMessage, "Initializing FT600 device...", sizeof(g_statusMessage));

    status = FT_CreateDeviceInfoList(&numDevices);
    if (FT_FAILED(status)) {
        sprintf(g_statusMessage, "Failed to create device list (Status: 0x%08X)", (unsigned int)status);
        return PX_FALSE;
    }
    if (numDevices == 0) {
        PX_strcpy(g_statusMessage, "No FT600 devices found. Please connect device.", sizeof(g_statusMessage));
        return PX_FALSE;
    }
    status = FT_Create(0, FT_OPEN_BY_INDEX, &g_ftHandle);
    if (FT_FAILED(status)) {
        sprintf(g_statusMessage, "Failed to open device (Status: 0x%08X)", (unsigned int)status);
        return PX_FALSE;
    }
    FT_SetPipeTimeout(g_ftHandle, PIPE_ID, 1000);
    FT_FlushPipe(g_ftHandle, PIPE_ID);
    FT_SetStreamPipe(g_ftHandle, FALSE, FALSE, PIPE_ID, BUFFER_SIZE);

    g_deviceConnected = PX_TRUE;
    g_dataDisplay.lastUpdateTime = PX_TimeGetTime();
    PX_strcpy(g_statusMessage, "Device ready, receiving data...", sizeof(g_statusMessage));
    return PX_TRUE;
}

/* USB receive thread */
DWORD WINAPI USBReceiveThread(LPVOID param) {
    UCHAR* buffer = (UCHAR*)malloc(BUFFER_SIZE);
    if (!buffer) {
        PX_strcpy(g_statusMessage, "ERROR: Failed to allocate buffer", sizeof(g_statusMessage));
        return 0;
    }
    g_threadRunning = PX_TRUE;
    PX_strcpy(g_statusMessage, "USB thread started, reading data...", sizeof(g_statusMessage));

    while (g_running && g_deviceConnected) {
        ULONG bytesTransferred = 0;
        FT_STATUS status = FT_ReadPipe(g_ftHandle, PIPE_ID, buffer, BUFFER_SIZE, &bytesTransferred, NULL);

        PX_MutexLock(&g_dataMutex);
        g_dataDisplay.readCallCount++;

        if (FT_SUCCESS(status) && bytesTransferred > 0) {
            g_dataDisplay.successCount++;
            g_dataDisplay.totalBytes += bytesTransferred;

            if (g_paused) {
                if (bytesTransferred >= 2) {
                    g_dataDisplay.lastValue = ParseDataValue(buffer[bytesTransferred - 2], buffer[bytesTransferred - 1]);
                }
                PX_MutexUnlock(&g_dataMutex);
                continue;
            }

            px_int localCounter = 0;
            px_dword sum = 0;
            px_short minVal = 32767, maxVal = -32768;

            for (ULONG i = 0; i < bytesTransferred - 1; i += 2) {
                px_short value = ParseDataValue(buffer[i], buffer[i+1]);

                /* clamp again before storing */
                if (g_raw16bit) {
                    if (value > 32767) value = 32767;
                    if (value < -32768) value = -32768;
                } else {
                    if (value > 2047) value = 2047;
                    if (value < -2048) value = -2048;
                }

                /* update pre-trigger rolling buffer */
                if (!g_dataDisplay.dataReady) {
                    g_dataDisplay.preTrigger[g_dataDisplay.preWrite] = value;
                    g_dataDisplay.preWrite = (g_dataDisplay.preWrite + 1) % PRE_TRIGGER_WINDOW;
                    if (g_dataDisplay.preCount < PRE_TRIGGER_WINDOW) g_dataDisplay.preCount++;
                }

                if (g_dataDisplay.capturing) {
                    if ((localCounter % g_sampleRate) == 0) {
                        px_int writePos = g_dataDisplay.writeIndex % MAX_STORAGE_POINTS;
                        g_dataDisplay.values[writePos] = value;
                        g_dataDisplay.writeIndex = (g_dataDisplay.writeIndex + 1) % MAX_STORAGE_POINTS;
                        g_dataDisplay.count++;
                        sum += value;
                        if (value < minVal) minVal = value;
                        if (value > maxVal) maxVal = value;
                    }
                    if (g_dataDisplay.count >= CAPTURE_TARGET) {
                        g_dataDisplay.capturing = PX_FALSE;
                        g_dataDisplay.dataReady = PX_TRUE;
                        g_dataDisplay.drawnOnce = PX_FALSE;
                        if (g_dataDisplay.count > 0) {
                            g_dataDisplay.minValue = minVal;
                            g_dataDisplay.maxValue = maxVal;
                            g_dataDisplay.avgValue = (px_float)sum / g_dataDisplay.count;
                        }
                        sprintf(g_statusMessage, "Capture complete: %d points ready", g_dataDisplay.count);
                        break;
                    }
                } else {
                    if (!g_dataDisplay.dataReady && g_triggerMode != TRIGGER_OFF) {
                        px_short prev = g_dataDisplay.lastValue;
                        px_bool initialCross = PX_FALSE;
                        TriggerMode detectedEdge = TRIGGER_OFF;

                        if (g_triggerMode == TRIGGER_RISING) {
                            if (prev < g_triggerLevel && value >= g_triggerLevel) { initialCross = PX_TRUE; detectedEdge = TRIGGER_RISING; }
                        } else if (g_triggerMode == TRIGGER_FALLING) {
                            if (prev > g_triggerLevel && value <= g_triggerLevel) { initialCross = PX_TRUE; detectedEdge = TRIGGER_FALLING; }
                        } else if (g_triggerMode == TRIGGER_AUTO) {
                            if (prev < g_triggerLevel && value >= g_triggerLevel) { initialCross = PX_TRUE; detectedEdge = TRIGGER_RISING; }
                            else if (prev > g_triggerLevel && value <= g_triggerLevel) { initialCross = PX_TRUE; detectedEdge = TRIGGER_FALLING; }
                        }

                        if (initialCross) {
                            g_dataDisplay.triggerPending = PX_TRUE;
                            g_dataDisplay.triggerConfirmCount = 1;
                            g_dataDisplay.triggerExpected = detectedEdge;
                        } else if (g_dataDisplay.triggerPending) {
                            if (g_dataDisplay.triggerExpected == TRIGGER_RISING) {
                                if (value >= g_triggerLevel) {
                                    g_dataDisplay.triggerConfirmCount++;
                                } else {
                                    g_dataDisplay.triggerPending = PX_FALSE;
                                    g_dataDisplay.triggerConfirmCount = 0;
                                }
                            } else if (g_dataDisplay.triggerExpected == TRIGGER_FALLING) {
                                if (value <= g_triggerLevel) {
                                    g_dataDisplay.triggerConfirmCount++;
                                } else {
                                    g_dataDisplay.triggerPending = PX_FALSE;
                                    g_dataDisplay.triggerConfirmCount = 0;
                                }
                            }
                            if (g_dataDisplay.triggerPending && g_dataDisplay.triggerConfirmCount >= PRE_TRIGGER_CONFIRM) {
                                g_dataDisplay.capturing = PX_TRUE;
                                g_dataDisplay.writeIndex = 0;
                                g_dataDisplay.count = 0;
                                g_dataDisplay.triggerPending = PX_FALSE;
                                g_dataDisplay.triggerConfirmCount = 0;
                                g_dataDisplay.triggerExpected = TRIGGER_OFF;
                                sprintf(g_statusMessage, "Trigger confirmed -> start capturing %d points", CAPTURE_TARGET);

                                if ((localCounter % g_sampleRate) == 0) {
                                    px_int writePos = g_dataDisplay.writeIndex % MAX_STORAGE_POINTS;
                                    g_dataDisplay.values[writePos] = value;
                                    g_dataDisplay.writeIndex = (g_dataDisplay.writeIndex + 1) % MAX_STORAGE_POINTS;
                                    g_dataDisplay.count++;
                                    sum += value;
                                    if (value < minVal) minVal = value;
                                    if (value > maxVal) maxVal = value;
                                }
                            }
                        }
                    }
                }

                g_dataDisplay.lastValue = value;
                localCounter++;
            } // end packet loop

            // update speed every second
            px_dword currentTime = PX_TimeGetTime();
            if (currentTime - g_dataDisplay.lastUpdateTime >= 1000) {
                px_dword bytesThisSecond = g_dataDisplay.totalBytes - g_dataDisplay.lastBytes;
                g_dataDisplay.currentSpeed = bytesThisSecond / (1024.0f * 1024.0f);
                g_dataDisplay.lastBytes = g_dataDisplay.totalBytes;
                g_dataDisplay.lastUpdateTime = currentTime;

                if (g_dataDisplay.dataReady) {
                    sprintf(g_statusMessage, "Capture ready: %d points (awaiting draw)", g_dataDisplay.count);
                } else if (g_dataDisplay.capturing) {
                    sprintf(g_statusMessage, "Capturing: %d/%d", g_dataDisplay.count, CAPTURE_TARGET);
                } else if (g_dataDisplay.triggerPending) {
                    sprintf(g_statusMessage, "Trigger pending (%d/%d)", g_dataDisplay.triggerConfirmCount, PRE_TRIGGER_CONFIRM);
                } else {
                    sprintf(g_statusMessage, "Waiting for trigger... PreCount=%d", g_dataDisplay.preCount);
                }
            }
        } else if (status == FT_TIMEOUT) {
            g_dataDisplay.timeoutCount++;
        } else if (status == FT_IO_PENDING) {
            // ignore
        } else {
            g_dataDisplay.errorCount++;
            sprintf(g_statusMessage, "USB Error: Status 0x%08X, Errors: %lu",
                    (unsigned int)status, (unsigned long)g_dataDisplay.errorCount);
        }

        PX_MutexUnlock(&g_dataMutex);
    } // while

    g_threadRunning = PX_FALSE;
    free(buffer);
    return 0;
}

/* Update */
px_void PX_ApplicationUpdate(PX_Application *pApp, px_dword elapsed) {
    static px_dword lastKeyTime = 0;
    px_dword currentTime = PX_TimeGetTime();
    if (currentTime - lastKeyTime < 200) return;

    if (GetAsyncKeyState(VK_SPACE) & 0x8000) { g_paused = !g_paused; lastKeyTime = currentTime; }

    if (GetAsyncKeyState(VK_UP) & 0x8000) {
        if (g_sampleRate > 1) {
            if (g_sampleRate >= 100) g_sampleRate -= 10;
            else if (g_sampleRate >= 10) g_sampleRate -= 5;
            else g_sampleRate -= 1;
        }
        lastKeyTime = currentTime;
    }
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
        if (g_sampleRate < 1000) {
            if (g_sampleRate >= 100) g_sampleRate += 10;
            else if (g_sampleRate >= 10) g_sampleRate += 5;
            else g_sampleRate += 1;
        }
        lastKeyTime = currentTime;
    }

    if (GetAsyncKeyState('T') & 0x8000) { g_triggerMode = (TriggerMode)((g_triggerMode + 1) % 4); lastKeyTime = currentTime; }
    if (GetAsyncKeyState('E') & 0x8000) { g_littleEndian = !g_littleEndian; lastKeyTime = currentTime; }
    if (GetAsyncKeyState('D') & 0x8000) { g_raw16bit = !g_raw16bit; lastKeyTime = currentTime; }
    if (GetAsyncKeyState('B') & 0x8000) { g_high12bit = !g_high12bit; lastKeyTime = currentTime; }

    if (GetAsyncKeyState('R') & 0x8000) {
        PX_MutexLock(&g_dataMutex);
        g_dataDisplay.writeIndex = 0; g_dataDisplay.count = 0; g_dataDisplay.capturing = PX_FALSE;
        g_dataDisplay.dataReady = PX_FALSE; g_dataDisplay.drawnOnce = PX_FALSE;
        g_dataDisplay.triggerPending = PX_FALSE; g_dataDisplay.triggerConfirmCount = 0;
        g_dataDisplay.preWrite = 0; g_dataDisplay.preCount = 0; g_dataDisplay.prevCount = 0;
        g_dataDisplay.lastValue = 0;
        PX_MutexUnlock(&g_dataMutex);
        lastKeyTime = currentTime;
    }

    if (GetAsyncKeyState(VK_LEFT) & 0x8000) { if (g_triggerLevel > -2048 + 10) g_triggerLevel -= 10; lastKeyTime = currentTime; }
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { if (g_triggerLevel < 2047 - 10) g_triggerLevel += 10; lastKeyTime = currentTime; }
}

/* Render */
px_void PX_ApplicationRender(PX_Application *pApp, px_dword elapsed) {
    px_surface *pSurface = &pApp->runtime.RenderSurface;
    px_char text[256];
    px_int surfaceWidth = pSurface->width;
    px_int surfaceHeight = pSurface->height;

    PX_SurfaceClear(pSurface, 0, 0, surfaceWidth, surfaceHeight, PX_COLOR(255, 20, 20, 30));
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, 10, 10, PX_ALIGN_LEFTTOP, "FT600 USB Data Visualization (fixed parsing/clamp)", PX_COLOR(255, 100, 200, 255));

    PX_MutexLock(&g_dataMutex);

    // debug info
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, 10, 40, PX_ALIGN_LEFTTOP, g_statusMessage, PX_COLOR(255, 255, 200, 100));
    sprintf(text, "Speed: %.2f MB/s", g_dataDisplay.currentSpeed);
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth - 10, 10, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 255, 255, 100));
    sprintf(text, "Total: %.2f MB", g_dataDisplay.totalBytes / (1024.0f * 1024.0f));
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth - 10, 35, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 255, 255, 100));

    sprintf(text, "Reads:%lu Success:%lu Timeout:%lu Error:%lu", (unsigned long)g_dataDisplay.readCallCount, (unsigned long)g_dataDisplay.successCount, (unsigned long)g_dataDisplay.timeoutCount, (unsigned long)g_dataDisplay.errorCount);
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, 10, 60, PX_ALIGN_LEFTTOP, text, PX_COLOR(255, 200, 200, 200));

    sprintf(text, "Stored: %d/%d | Prev:%d", g_dataDisplay.count, CAPTURE_TARGET, g_dataDisplay.prevCount);
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth - 10, 60, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 255, 255, 100));

    sprintf(text, "Sample Rate: 1:%d | Y-Scale:+/-%d", g_sampleRate, g_yScale);
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth - 10, 85, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 255, 255, 100));

    const char* triggerStr[] = {"OFF","RISING","FALLING","AUTO"};
    sprintf(text, "Trigger: %s @ %d | Pending:%d Confirm:%d PreCnt:%d", triggerStr[g_triggerMode], g_triggerLevel, g_dataDisplay.triggerPending ? 1 : 0, g_dataDisplay.triggerConfirmCount, g_dataDisplay.preCount);
    PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth - 10, 110, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 255, 255, 100));

    px_int waveformHeight = surfaceHeight - 320;
    px_int waveformY = 100;
    px_int waveformWidth = surfaceWidth - 40;
    px_short yMin = -g_yScale, yMax = g_yScale;
    float yRangeF = (float)(yMax - yMin);

    for (int i=0;i<=4;i++) {
        px_int y = waveformY + (waveformHeight / 4) * i;
        PX_GeoDrawLine(pSurface, 20, y, 20 + waveformWidth, y, 1, PX_COLOR(255, 40, 40, 40));
        px_int labelValue = yMax - ( (yMax - yMin) * i / 4 );
        sprintf(text, "%d", labelValue);
        PX_FontModuleDrawText(pSurface, pApp->pfontmodule, 15, y, PX_ALIGN_RIGHTTOP, text, PX_COLOR(255, 150, 150, 150));
    }
    for (int i=0;i<=10;i++) {
        px_int x = 20 + (waveformWidth / 10) * i;
        PX_GeoDrawLine(pSurface, x, waveformY, x, waveformY + waveformHeight, 1, PX_COLOR(255, 40, 40, 40));
    }

    px_int zeroY = waveformY + waveformHeight/2;
    PX_GeoDrawLine(pSurface, 20, zeroY, 20+waveformWidth, zeroY, 1, PX_COLOR(255, 100,100,100));

    // Draw prev (faded)
    if (g_dataDisplay.prevCount > 1) {
        int displayCount = g_dataDisplay.prevCount > MAX_DISPLAY_POINTS ? MAX_DISPLAY_POINTS : g_dataDisplay.prevCount;
        for (int i=0;i<displayCount-1;i++) {
            int idx1 = (int)((long long)i * g_dataDisplay.prevCount / (displayCount - 1));
            int idx2 = (int)((long long)(i+1) * g_dataDisplay.prevCount / (displayCount - 1));
            if (idx1 >= g_dataDisplay.prevCount) idx1 = g_dataDisplay.prevCount-1;
            if (idx2 >= g_dataDisplay.prevCount) idx2 = g_dataDisplay.prevCount-1;

            float v1 = (float)g_dataDisplay.prevValues[idx1];
            float v2 = (float)g_dataDisplay.prevValues[idx2];

            if (v1 > yMax) { v1 = yMax; }
            if (v1 < yMin) { v1 = yMin; }
            if (v2 > yMax) { v2 = yMax; }
            if (v2 < yMin) { v2 = yMin; }

            int x1 = 20 + (waveformWidth * i) / (displayCount - 1);
            int x2 = 20 + (waveformWidth * (i+1)) / (displayCount - 1);
            int y1 = (int)( waveformY + waveformHeight - ((v1 - (float)yMin) * (float)waveformHeight / yRangeF) );
            int y2 = (int)( waveformY + waveformHeight - ((v2 - (float)yMin) * (float)waveformHeight / yRangeF) );

            if (y1 < waveformY) { y1 = waveformY; }
            if (y1 > waveformY + waveformHeight) { y1 = waveformY + waveformHeight; }
            if (y2 < waveformY) { y2 = waveformY; }
            if (y2 > waveformY + waveformHeight) { y2 = waveformY + waveformHeight; }

            PX_GeoDrawLine(pSurface, x1, y1, x2, y2, 1, PX_COLOR(180, 120, 120, 120));
        }
    }

    // Draw current capture if ready
    if (g_dataDisplay.dataReady && g_dataDisplay.count > 1) {
        int displayCount = g_dataDisplay.count > MAX_DISPLAY_POINTS ? MAX_DISPLAY_POINTS : g_dataDisplay.count;
        int lastIdx = (g_dataDisplay.writeIndex - 1 + MAX_STORAGE_POINTS) % MAX_STORAGE_POINTS;
        int firstIdx = (lastIdx - (g_dataDisplay.count - 1) + MAX_STORAGE_POINTS) % MAX_STORAGE_POINTS;

        for (int i=0;i<displayCount-1;i++) {
            int pos1 = firstIdx + (int)((long long)i * (g_dataDisplay.count - 1) / (displayCount - 1));
            int pos2 = firstIdx + (int)((long long)(i+1) * (g_dataDisplay.count - 1) / (displayCount - 1));
            pos1 = (pos1 % MAX_STORAGE_POINTS + MAX_STORAGE_POINTS) % MAX_STORAGE_POINTS;
            pos2 = (pos2 % MAX_STORAGE_POINTS + MAX_STORAGE_POINTS) % MAX_STORAGE_POINTS;

            float v1 = (float)g_dataDisplay.values[pos1];
            float v2 = (float)g_dataDisplay.values[pos2];

            if (v1 > yMax) { v1 = yMax; }
            if (v1 < yMin) { v1 = yMin; }
            if (v2 > yMax) { v2 = yMax; }
            if (v2 < yMin) { v2 = yMin; }

            int x1 = 20 + (waveformWidth * i) / (displayCount - 1);
            int x2 = 20 + (waveformWidth * (i+1)) / (displayCount - 1);
            int y1 = (int)( waveformY + waveformHeight - ((v1 - (float)yMin) * (float)waveformHeight / yRangeF) );
            int y2 = (int)( waveformY + waveformHeight - ((v2 - (float)yMin) * (float)waveformHeight / yRangeF) );

            if (y1 < waveformY) { y1 = waveformY; }
            if (y1 > waveformY + waveformHeight) { y1 = waveformY + waveformHeight; }
            if (y2 < waveformY) { y2 = waveformY; }
            if (y2 > waveformY + waveformHeight) { y2 = waveformY + waveformHeight; }

            PX_GeoDrawLine(pSurface, x1, y1, x2, y2, 2, PX_COLOR(255, 0, 255, 255));
        }

        // copy captured block into prevValues (linear)
        if (g_dataDisplay.count <= MAX_STORAGE_POINTS) {
            px_int c = g_dataDisplay.count;
            px_int src = firstIdx;
            for (int i=0;i<c;i++) {
                g_dataDisplay.prevValues[i] = g_dataDisplay.values[src];
                src = (src + 1) % MAX_STORAGE_POINTS;
            }
            g_dataDisplay.prevCount = c;
        } else {
            g_dataDisplay.prevCount = 0;
        }

        g_dataDisplay.dataReady = PX_FALSE;
        g_dataDisplay.drawnOnce = PX_TRUE;
        g_dataDisplay.writeIndex = 0;
        g_dataDisplay.count = 0;
        g_dataDisplay.capturing = PX_FALSE;
        sprintf(g_statusMessage, "Frame drawn and preserved (prevCount=%d).", g_dataDisplay.prevCount);
    } else {
        if (g_paused) {
            PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth/2, surfaceHeight/2, PX_ALIGN_CENTER, "Paused - previous waveform retained. Press SPACE to resume.", PX_COLOR(255, 200, 200, 200));
        } else {
            PX_FontModuleDrawText(pSurface, pApp->pfontmodule, surfaceWidth/2, surfaceHeight/2, PX_ALIGN_CENTER, "Waiting for trigger... previous waveform retained", PX_COLOR(255,200,200,200));
        }
    }

    PX_MutexUnlock(&g_dataMutex);
}

/* main */
int px_main() {
    PX_MutexInitialize(&g_dataMutex);
    PainterEngine_Initialize(1024,768);

    PX_MutexLock(&g_dataMutex);
    g_dataDisplay.writeIndex = 0; g_dataDisplay.count = 0; g_dataDisplay.preWrite = 0; g_dataDisplay.preCount = 0;
    g_dataDisplay.capturing = PX_FALSE; g_dataDisplay.dataReady = PX_FALSE; g_dataDisplay.drawnOnce = PX_FALSE;
    g_dataDisplay.triggerPending = PX_FALSE; g_dataDisplay.triggerConfirmCount = 0; g_dataDisplay.prevCount = 0;
    g_dataDisplay.lastValue = 0;
    PX_MutexUnlock(&g_dataMutex);

    if (InitializeFT600()) {
        HANDLE hThread = CreateThread(NULL, 0, USBReceiveThread, NULL, 0, NULL);
        if (!hThread) {
            PX_strcpy(g_statusMessage, "ERROR: Failed to create USB thread", sizeof(g_statusMessage));
        } else {
            PX_strcpy(g_statusMessage, "USB thread created successfully", sizeof(g_statusMessage));
        }
    }
    return 0;
}
