/**
 * FT600 USB3.0 Data Reception Speed Test Application
 *
 * This program tests the data reception speed from FT600 chip
 * and displays the throughput in MB/s
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "FTD3XX.h"
#include "PainterEngine.h"

// Configuration
#define BUFFER_SIZE         (1024 * 1024)      // 1MB buffer
#define TEST_DURATION_SEC   10                  // Test duration in seconds
#define PIPE_ID             0x82                // IN endpoint pipe ID (typically 0x82 for FT600)

// Statistics structure
typedef struct {
    ULONGLONG totalBytes;
    double totalTime;
    double avgSpeed;
    double maxSpeed;
    double minSpeed;
} SpeedStats;

/**
 * Display error message with FT status code
 */
void printError(const char* message, FT_STATUS status) {
    fprintf(stderr, "Error: %s (Status: 0x%08lX)\n", message, (unsigned long)status);
}

/**
 * Get elapsed time in seconds
 */
double getElapsedTime(clock_t start) {
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

/**
 * Format byte size to human readable format
 */
void formatBytes(ULONGLONG bytes, char* buffer, size_t bufSize) {
    if (bytes < 1024) {
        snprintf(buffer, bufSize, "%llu B", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buffer, bufSize, "%.2f KB", bytes / 1024.0);
    } else if (bytes < 1024 * 1024 * 1024) {
        snprintf(buffer, bufSize, "%.2f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buffer, bufSize, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

/**
 * Initialize and open FT600 device
 */
FT_STATUS openDevice(FT_HANDLE* ftHandle) {
    FT_STATUS status;
    DWORD numDevices = 0;

    printf("Initializing FT600 device...\n");

    // Create device info list
    status = FT_CreateDeviceInfoList(&numDevices);
    if (FT_FAILED(status)) {
        printError("Failed to create device info list", status);
        return status;
    }

    if (numDevices == 0) {
        fprintf(stderr, "Error: No FT600 devices found\n");
        return FT_DEVICE_NOT_FOUND;
    }

    printf("Found %lu device(s)\n", (unsigned long)numDevices);

    // Open first device by index
    status = FT_Create(0, FT_OPEN_BY_INDEX, ftHandle);
    if (FT_FAILED(status)) {
        printError("Failed to open device", status);
        return status;
    }

    printf("Device opened successfully\n");

    // Get device info
    USHORT vid, pid;
    status = FT_GetVIDPID(*ftHandle, &vid, &pid);
    if (FT_SUCCESS(status)) {
        printf("Device VID: 0x%04X, PID: 0x%04X\n", vid, pid);
    }

    // Get firmware version
    ULONG firmwareVersion;
    status = FT_GetFirmwareVersion(*ftHandle, &firmwareVersion);
    if (FT_SUCCESS(status)) {
        printf("Firmware Version: 0x%08lX\n", (unsigned long)firmwareVersion);
    }

    return FT_OK;
}

/**
 * Display pipe information
 */
void displayPipeInfo(FT_HANDLE ftHandle) {
    FT_STATUS status;
    FT_PIPE_INFORMATION pipeInfo;

    printf("\nQuerying pipe information...\n");

    // Query pipes on interface 0 (typically data interface)
    for (UCHAR pipeIndex = 0; pipeIndex < 4; pipeIndex++) {
        status = FT_GetPipeInformation(ftHandle, 0, pipeIndex, &pipeInfo);
        if (FT_SUCCESS(status)) {
            printf("  Pipe %d: ID=0x%02X, Type=%d, MaxPacketSize=%d, %s\n",
                   pipeIndex,
                   pipeInfo.PipeId,
                   pipeInfo.PipeType,
                   pipeInfo.MaximumPacketSize,
                   FT_IS_READ_PIPE(pipeInfo.PipeId) ? "IN (Read)" : "OUT (Write)");
        }
    }
}

/**
 * Configure FT600 for optimal throughput
 */
FT_STATUS configureDevice(FT_HANDLE ftHandle) {
    FT_STATUS status;

    printf("\nConfiguring device...\n");

    // Display available pipes
    displayPipeInfo(ftHandle);

    // Set pipe timeout (5 seconds)
    status = FT_SetPipeTimeout(ftHandle, PIPE_ID, 5000);
    if (FT_FAILED(status)) {
        printf("Warning: Failed to set pipe timeout (Status: 0x%08lX)\n", (unsigned long)status);
        // This is not critical, continue anyway
    } else {
        printf("Pipe timeout set successfully\n");
    }

    // Flush the pipe to clear any pending data
    status = FT_FlushPipe(ftHandle, PIPE_ID);
    if (FT_FAILED(status)) {
        printf("Warning: Failed to flush pipe (Status: 0x%08lX)\n", (unsigned long)status);
        // This is not critical, continue anyway
    } else {
        printf("Pipe flushed successfully\n");
    }

    // Set stream pipe for better performance
    status = FT_SetStreamPipe(ftHandle, FALSE, FALSE, PIPE_ID, BUFFER_SIZE);
    if (FT_FAILED(status)) {
        printf("Warning: Failed to set stream pipe (Status: 0x%08lX)\n", (unsigned long)status);
        // This is not critical, continue anyway
    } else {
        printf("Stream pipe configured successfully\n");
    }

    printf("\nDevice configuration completed\n");
    return FT_OK;
}

/**
 * Perform speed test
 */
void performSpeedTest(FT_HANDLE ftHandle) {
    FT_STATUS status;
    UCHAR* buffer;
    ULONG bytesTransferred;
    clock_t startTime, lastReportTime;
    double elapsedTime;
    SpeedStats stats = {0};
    ULONGLONG lastBytes = 0;

    // Allocate receive buffer
    buffer = (UCHAR*)malloc(BUFFER_SIZE);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate buffer\n");
        return;
    }

    printf("\n========================================\n");
    printf("Starting speed test for %d seconds...\n", TEST_DURATION_SEC);
    printf("Buffer size: %d bytes\n", BUFFER_SIZE);
    printf("Pipe ID: 0x%02X\n", PIPE_ID);
    printf("========================================\n\n");

    stats.minSpeed = 999999.0;  // Initialize with large value
    startTime = clock();
    lastReportTime = startTime;

    // Main receive loop
    while (1) {
        // Read data from pipe
        status = FT_ReadPipe(ftHandle, PIPE_ID, buffer, BUFFER_SIZE, &bytesTransferred, NULL);

        if (FT_SUCCESS(status)) {
            stats.totalBytes += bytesTransferred;
        } else if (status == FT_TIMEOUT) {
            // Timeout is not an error, just no data available
            continue;
        } else if (status == FT_IO_PENDING) {
            // Operation still in progress
            continue;
        } else {
            printError("Read pipe failed", status);
            break;
        }

        elapsedTime = getElapsedTime(startTime);

        // Report every second
        if (getElapsedTime(lastReportTime) >= 1.0) {
            ULONGLONG bytesThisSecond = stats.totalBytes - lastBytes;
            double currentSpeed = bytesThisSecond / (1024.0 * 1024.0);  // MB/s
            char totalStr[64];

            // Update statistics
            if (currentSpeed > stats.maxSpeed) stats.maxSpeed = currentSpeed;
            if (currentSpeed < stats.minSpeed && currentSpeed > 0) stats.minSpeed = currentSpeed;

            formatBytes(stats.totalBytes, totalStr, sizeof(totalStr));

            printf("[%.1fs] Speed: %.2f MB/s | Total: %s\n",
                   elapsedTime, currentSpeed, totalStr);

            lastBytes = stats.totalBytes;
            lastReportTime = clock();
        }

        // Check if test duration completed
        if (elapsedTime >= TEST_DURATION_SEC) {
            break;
        }
    }

    // Calculate final statistics
    stats.totalTime = getElapsedTime(startTime);
    stats.avgSpeed = (stats.totalBytes / stats.totalTime) / (1024.0 * 1024.0);

    // Display results
    char totalStr[64];
    formatBytes(stats.totalBytes, totalStr, sizeof(totalStr));

    printf("\n========================================\n");
    printf("Test Results:\n");
    printf("========================================\n");
    printf("Total time:     %.2f seconds\n", stats.totalTime);
    printf("Total bytes:    %s (%llu bytes)\n", totalStr, stats.totalBytes);
    printf("Average speed:  %.2f MB/s\n", stats.avgSpeed);
    printf("Max speed:      %.2f MB/s\n", stats.maxSpeed);
    if (stats.minSpeed < 999999.0) {
        printf("Min speed:      %.2f MB/s\n", stats.minSpeed);
    }
    printf("Throughput:     %.2f Mbps\n", stats.avgSpeed * 8);
    printf("========================================\n");

    free(buffer);
}

int main(int argc, char* argv[]);

// WinMain wrapper for Windows compatibility
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    return main(__argc, __argv);
}

/**
 * Main function
 */
int main(int argc, char* argv[]) {
    FT_STATUS status;
    FT_HANDLE ftHandle = NULL;

    printf("========================================\n");
    printf("FT600 USB3.0 Speed Test Application\n");
    printf("========================================\n\n");

    // Open device
    status = openDevice(&ftHandle);
    if (FT_FAILED(status)) {
        fprintf(stderr, "Failed to open device. Exiting.\n");
        fflush(stderr);
        printf("\nPress Enter to exit...");
        fflush(stdout);
        getchar();
        return EXIT_FAILURE;
    }

    // Configure device
    status = configureDevice(ftHandle);
    if (FT_FAILED(status)) {
        fprintf(stderr, "Failed to configure device. Exiting.\n");
        fflush(stderr);
        FT_Close(ftHandle);
        printf("\nPress Enter to exit...");
        fflush(stdout);
        getchar();
        return EXIT_FAILURE;
    }

    // Perform speed test
    performSpeedTest(ftHandle);

    // Cleanup
    printf("\nClosing device...\n");

    // Clear stream pipe
    FT_ClearStreamPipe(ftHandle, FALSE, FALSE, PIPE_ID);

    // Close device
    status = FT_Close(ftHandle);
    if (FT_FAILED(status)) {
        printError("Failed to close device", status);
        printf("\nPress Enter to exit...");
        getchar();
        return EXIT_FAILURE;
    }

    printf("Device closed successfully\n");
    printf("\nPress Enter to exit...");
    getchar();

    return EXIT_SUCCESS;
}
