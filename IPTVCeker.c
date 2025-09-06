#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wininet.h>
#include <time.h>
#include <tchar.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comctl32.lib")

#define MAX_URL_LENGTH 1024
#define MAX_LINES 10000
#define MAX_THREADS 50
#define ID_BROWSE 101
#define ID_START 102
#define ID_STOP 103
#define ID_SAVE 104
#define ID_WORKERS_SPIN 105
#define ID_TIMEOUT_SPIN 106
#define ID_OUTPUT_EDIT 107
#define ID_FILE_EDIT 108
#define ID_LOG_EDIT 109
#define ID_RESULTS_EDIT 110
#define ID_PROGRESS_BAR 111
#define ID_WORKERS_EDIT 112
#define ID_TIMEOUT_EDIT 113

typedef struct {
    char url[MAX_URL_LENGTH];
    char status[50];
    double response_time;
} CheckResult;

typedef struct {
    char** urls;
    int count;
    int timeout_ms;
    int max_workers;
    CheckResult* results;
    volatile long current_index;
    volatile long processed;
    HANDLE mutex;
    HWND hwnd;
    volatile BOOL stop_requested;
} ThreadData;

typedef struct {
    HWND hFileEdit;
    HWND hBrowseBtn;
    HWND hWorkersSpin;
    HWND hWorkersEdit;
    HWND hTimeoutSpin;
    HWND hTimeoutEdit;
    HWND hOutputEdit;
    HWND hStartBtn;
    HWND hStopBtn;
    HWND hSaveBtn;
    HWND hProgressBar;
    HWND hLogEdit;
    HWND hResultsEdit;
    HWND hTotalLabel;
    HWND hOnlineLabel;
    HWND hOfflineLabel;
    
    ThreadData* thread_data;
    HANDLE hCheckThread;
    int online_count;
    int offline_count;
} GUIData;

// Function prototypes
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreateMainWindow(HWND);
void BrowseForFile(HWND);
void StartChecking(HWND);
void StopChecking(HWND);
void SaveResults(HWND);
void UpdateProgress(HWND, int, int, const char*, const char*, double);
void OnCheckingFinished(HWND, int, int);
char** read_urls_from_file(const char* filename, int* count);
CheckResult check_url(const char* url, int timeout_ms);
DWORD WINAPI CheckThreadProc(LPVOID param);
DWORD WINAPI check_url_thread(LPVOID param);
int is_valid_url(const char* url);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    static TCHAR szAppName[] = TEXT("IPTVChecker");
    HWND hwnd;
    MSG msg;
    WNDCLASSEX wndclass;

    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icex);

    wndclass.cbSize = sizeof(wndclass);
    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = WndProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = sizeof(GUIData*);
    wndclass.hInstance = hInstance;
    wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = szAppName;
    wndclass.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    RegisterClassEx(&wndclass);

    hwnd = CreateWindow(szAppName, TEXT("IPTV Checker - URL Validation Tool"),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, iCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static GUIData* gui_data = NULL;

    switch (message) {
    case WM_CREATE:
        gui_data = (GUIData*)malloc(sizeof(GUIData));
        ZeroMemory(gui_data, sizeof(GUIData));
        SetWindowLongPtr(hwnd, 0, (LONG_PTR)gui_data);
        CreateMainWindow(hwnd);
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BROWSE:
            BrowseForFile(hwnd);
            break;
        case ID_START:
            StartChecking(hwnd);
            break;
        case ID_STOP:
            StopChecking(hwnd);
            break;
        case ID_SAVE:
            SaveResults(hwnd);
            break;
        }
        break;

    case WM_DESTROY:
        if (gui_data && gui_data->thread_data) {
            gui_data->thread_data->stop_requested = TRUE;
            if (gui_data->hCheckThread) {
                WaitForSingleObject(gui_data->hCheckThread, 1000);
                CloseHandle(gui_data->hCheckThread);
            }
            free(gui_data->thread_data);
        }
        free(gui_data);
        PostQuitMessage(0);
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

void CreateMainWindow(HWND hwnd) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);

    // Settings group
    CreateWindow(TEXT("BUTTON"), TEXT("Check Settings"), 
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        10, 10, 960, 100, hwnd, NULL, hInstance, NULL);

    // File field
    CreateWindow(TEXT("STATIC"), TEXT("URLs File:"), 
        WS_CHILD | WS_VISIBLE,
        20, 30, 100, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hFileEdit = CreateWindow(TEXT("EDIT"), TEXT(""), 
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER,
        120, 30, 600, 20, hwnd, (HMENU)ID_FILE_EDIT, hInstance, NULL);

    gui_data->hBrowseBtn = CreateWindow(TEXT("BUTTON"), TEXT("Browse..."), 
        WS_CHILD | WS_VISIBLE,
        730, 30, 80, 20, hwnd, (HMENU)ID_BROWSE, hInstance, NULL);

    // Threads count
    CreateWindow(TEXT("STATIC"), TEXT("Threads:"), 
        WS_CHILD | WS_VISIBLE,
        20, 60, 50, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hWorkersEdit = CreateWindow(TEXT("EDIT"), TEXT("20"), 
        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_BORDER,
        70, 60, 40, 20, hwnd, (HMENU)ID_WORKERS_EDIT, hInstance, NULL);

    // Timeout
    CreateWindow(TEXT("STATIC"), TEXT("Timeout (sec):"), 
        WS_CHILD | WS_VISIBLE,
        120, 60, 80, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hTimeoutEdit = CreateWindow(TEXT("EDIT"), TEXT("5"), 
        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_BORDER,
        200, 60, 40, 20, hwnd, (HMENU)ID_TIMEOUT_EDIT, hInstance, NULL);

    // Output file
    CreateWindow(TEXT("STATIC"), TEXT("Output file:"), 
        WS_CHILD | WS_VISIBLE,
        250, 60, 80, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hOutputEdit = CreateWindow(TEXT("EDIT"), TEXT("online_links"), 
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        330, 60, 100, 20, hwnd, (HMENU)ID_OUTPUT_EDIT, hInstance, NULL);

    // Control buttons
    gui_data->hStartBtn = CreateWindow(TEXT("BUTTON"), TEXT("Start Check"), 
        WS_CHILD | WS_VISIBLE,
        10, 120, 120, 30, hwnd, (HMENU)ID_START, hInstance, NULL);

    gui_data->hStopBtn = CreateWindow(TEXT("BUTTON"), TEXT("Stop"), 
        WS_CHILD | WS_VISIBLE | WS_DISABLED,
        140, 120, 100, 30, hwnd, (HMENU)ID_STOP, hInstance, NULL);

    gui_data->hSaveBtn = CreateWindow(TEXT("BUTTON"), TEXT("Save Results"), 
        WS_CHILD | WS_VISIBLE | WS_DISABLED,
        250, 120, 100, 30, hwnd, (HMENU)ID_SAVE, hInstance, NULL);

    // Progress bar
    gui_data->hProgressBar = CreateWindow(PROGRESS_CLASS, TEXT(""), 
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        360, 125, 300, 20, hwnd, (HMENU)ID_PROGRESS_BAR, hInstance, NULL);

    // Statistics
    CreateWindow(TEXT("STATIC"), TEXT("Total:"), 
        WS_CHILD | WS_VISIBLE,
        670, 120, 40, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hTotalLabel = CreateWindow(TEXT("STATIC"), TEXT("0"), 
        WS_CHILD | WS_VISIBLE,
        710, 120, 40, 20, hwnd, NULL, hInstance, NULL);

    CreateWindow(TEXT("STATIC"), TEXT("Online:"), 
        WS_CHILD | WS_VISIBLE,
        760, 120, 50, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hOnlineLabel = CreateWindow(TEXT("STATIC"), TEXT("0"), 
        WS_CHILD | WS_VISIBLE,
        810, 120, 40, 20, hwnd, NULL, hInstance, NULL);

    CreateWindow(TEXT("STATIC"), TEXT("Offline:"), 
        WS_CHILD | WS_VISIBLE,
        860, 120, 50, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hOfflineLabel = CreateWindow(TEXT("STATIC"), TEXT("0"), 
        WS_CHILD | WS_VISIBLE,
        910, 120, 40, 20, hwnd, NULL, hInstance, NULL);

    // Check log
    CreateWindow(TEXT("STATIC"), TEXT("Check Log:"), 
        WS_CHILD | WS_VISIBLE,
        10, 160, 100, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hLogEdit = CreateWindow(TEXT("EDIT"), TEXT(""), 
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | WS_BORDER,
        10, 180, 960, 200, hwnd, (HMENU)ID_LOG_EDIT, hInstance, NULL);

    // Results
    CreateWindow(TEXT("STATIC"), TEXT("Results:"), 
        WS_CHILD | WS_VISIBLE,
        10, 390, 100, 20, hwnd, NULL, hInstance, NULL);

    gui_data->hResultsEdit = CreateWindow(TEXT("EDIT"), TEXT(""), 
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | WS_BORDER,
        10, 410, 960, 200, hwnd, (HMENU)ID_RESULTS_EDIT, hInstance, NULL);
}

void BrowseForFile(HWND hwnd) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    OPENFILENAME ofn;
    TCHAR szFile[MAX_PATH] = TEXT("");
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = TEXT("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    
    if (GetOpenFileName(&ofn)) {
        SetWindowText(gui_data->hFileEdit, szFile);
    }
}

DWORD WINAPI CheckThreadProc(LPVOID param) {
    ThreadData* data = (ThreadData*)param;
    
    data->current_index = 0;
    data->processed = 0;
    data->mutex = CreateMutex(NULL, FALSE, NULL);
    
    int num_threads = (data->max_workers < data->count) ? data->max_workers : data->count;
    HANDLE* threads = malloc(num_threads * sizeof(HANDLE));
    
    for (int i = 0; i < num_threads; i++) {
        threads[i] = CreateThread(NULL, 0, check_url_thread, data, 0, NULL);
    }
    
    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    
    for (int i = 0; i < num_threads; i++) {
        CloseHandle(threads[i]);
    }
    free(threads);
    CloseHandle(data->mutex);
    
    // Count results
    int online_count = 0;
    for (int i = 0; i < data->count; i++) {
        if (strstr(data->results[i].status, "Online")) {
            online_count++;
        }
    }
    
    OnCheckingFinished(data->hwnd, online_count, data->count - online_count);
    
    return 0;
}

DWORD WINAPI check_url_thread(LPVOID param) {
    ThreadData* data = (ThreadData*)param;
    
    while (!data->stop_requested) {
        WaitForSingleObject(data->mutex, INFINITE);
        int index = data->current_index;
        if (index >= data->count) {
            ReleaseMutex(data->mutex);
            break;
        }
        data->current_index++;
        ReleaseMutex(data->mutex);

        char* url = data->urls[index];
        CheckResult result = check_url(url, data->timeout_ms);
        data->results[index] = result;
        
        InterlockedIncrement(&data->processed);
        
        // Send message to GUI thread
        char message[256];
        sprintf(message, "[%ld/%d] %s - %s (%.2fms)", 
               data->processed, data->count, url, result.status, result.response_time);
        
        UpdateProgress(data->hwnd, data->processed, data->count, url, result.status, result.response_time);
    }
    
    return 0;
}

void UpdateProgress(HWND hwnd, int current, int total, const char* url, const char* status, double response_time) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    char message[512];
    sprintf(message, "[%d/%d] %s - %s (%.2fms)\r\n", current, total, url, status, response_time);
    
    // Add to log
    int length = GetWindowTextLength(gui_data->hLogEdit);
    SendMessage(gui_data->hLogEdit, EM_SETSEL, length, length);
    SendMessage(gui_data->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)message);
    
    // Update statistics
    if (strstr(status, "Online")) {
        gui_data->online_count++;
    } else {
        gui_data->offline_count++;
    }
    
    char count_str[20];
    sprintf(count_str, "%d", total);
    SetWindowTextA(gui_data->hTotalLabel, count_str);
    
    sprintf(count_str, "%d", gui_data->online_count);
    SetWindowTextA(gui_data->hOnlineLabel, count_str);
    
    sprintf(count_str, "%d", gui_data->offline_count);
    SetWindowTextA(gui_data->hOfflineLabel, count_str);
    
    // Update progress bar
    SendMessage(gui_data->hProgressBar, PBM_SETPOS, (current * 100) / total, 0);
}

void OnCheckingFinished(HWND hwnd, int online_count, int offline_count) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    // Enable buttons
    EnableWindow(gui_data->hStartBtn, TRUE);
    EnableWindow(gui_data->hStopBtn, FALSE);
    EnableWindow(gui_data->hSaveBtn, TRUE);
    
    // Show results
    char results[1024];
    sprintf(results, "CHECK COMPLETED\r\n"
                    "Total checked: %d\r\n"
                    "Online: %d (%.1f%%)\r\n"
                    "Offline: %d (%.1f%%)\r\n\r\n"
                    "WORKING LINKS:\r\n"
                    "==================================================\r\n",
                    online_count + offline_count, online_count, 
                    (double)online_count/(online_count+offline_count)*100,
                    offline_count,
                    (double)offline_count/(online_count+offline_count)*100);
    
    SetWindowTextA(gui_data->hResultsEdit, results);
    
    // Add working links
    for (int i = 0; i < gui_data->thread_data->count; i++) {
        if (strstr(gui_data->thread_data->results[i].status, "Online")) {
            char line[MAX_URL_LENGTH + 10];
            sprintf(line, "%s\r\n", gui_data->thread_data->results[i].url);
            
            int length = GetWindowTextLength(gui_data->hResultsEdit);
            SendMessage(gui_data->hResultsEdit, EM_SETSEL, length, length);
            SendMessage(gui_data->hResultsEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
        }
    }
}

void StartChecking(HWND hwnd) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    // Get parameters using ANSI version
    char file_path[MAX_PATH];
    GetWindowTextA(gui_data->hFileEdit, file_path, MAX_PATH);
    
    if (strlen(file_path) == 0) {
        MessageBoxA(hwnd, "Please select URLs file!", "Error", MB_ICONERROR);
        return;
    }
    
    char workers_str[10], timeout_str[10];
    GetWindowTextA(gui_data->hWorkersEdit, workers_str, 10);
    GetWindowTextA(gui_data->hTimeoutEdit, timeout_str, 10);
    
    int max_workers = atoi(workers_str);
    int timeout_sec = atoi(timeout_str);
    
    if (max_workers < 1 || max_workers > MAX_THREADS) {
        MessageBoxA(hwnd, "Invalid threads count!", "Error", MB_ICONERROR);
        return;
    }
    
    if (timeout_sec < 1 || timeout_sec > 60) {
        MessageBoxA(hwnd, "Timeout must be between 1 and 60 seconds!", "Error", MB_ICONERROR);
        return;
    }
    
    // Read URLs from file
    int url_count = 0;
    char** urls = read_urls_from_file(file_path, &url_count);
    
    if (!urls || url_count == 0) {
        MessageBoxA(hwnd, "No valid URLs found in file!", "Error", MB_ICONERROR);
        return;
    }
    
    // Initialize data structure
    gui_data->thread_data = (ThreadData*)malloc(sizeof(ThreadData));
    ZeroMemory(gui_data->thread_data, sizeof(ThreadData));
    
    gui_data->thread_data->urls = urls;
    gui_data->thread_data->count = url_count;
    gui_data->thread_data->timeout_ms = timeout_sec * 1000;
    gui_data->thread_data->max_workers = max_workers;
    gui_data->thread_data->results = malloc(url_count * sizeof(CheckResult));
    gui_data->thread_data->hwnd = hwnd;
    gui_data->thread_data->stop_requested = FALSE;
    
    // Reset interface
    SetWindowText(gui_data->hLogEdit, TEXT(""));
    SetWindowText(gui_data->hResultsEdit, TEXT(""));
    SetWindowText(gui_data->hTotalLabel, TEXT("0"));
    SetWindowText(gui_data->hOnlineLabel, TEXT("0"));
    SetWindowText(gui_data->hOfflineLabel, TEXT("0"));
    
    SendMessage(gui_data->hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(gui_data->hProgressBar, PBM_SETPOS, 0, 0);
    
    gui_data->online_count = 0;
    gui_data->offline_count = 0;
    
    // Disable buttons
    EnableWindow(gui_data->hStartBtn, FALSE);
    EnableWindow(gui_data->hStopBtn, TRUE);
    EnableWindow(gui_data->hSaveBtn, FALSE);
    
    // Start check thread
    gui_data->hCheckThread = CreateThread(NULL, 0, CheckThreadProc, gui_data->thread_data, 0, NULL);
}

void StopChecking(HWND hwnd) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    if (gui_data->thread_data) {
        gui_data->thread_data->stop_requested = TRUE;
        
        EnableWindow(gui_data->hStartBtn, TRUE);
        EnableWindow(gui_data->hStopBtn, FALSE);
        
        // Add message to log
        int length = GetWindowTextLength(gui_data->hLogEdit);
        SendMessage(gui_data->hLogEdit, EM_SETSEL, length, length);
        SendMessage(gui_data->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)"Check stopped by user\r\n");
    }
}

void SaveResults(HWND hwnd) {
    GUIData* gui_data = (GUIData*)GetWindowLongPtr(hwnd, 0);
    
    if (!gui_data->thread_data) {
        MessageBoxA(hwnd, "No data to save!", "Error", MB_ICONERROR);
        return;
    }
    
    char output_prefix[256];
    GetWindowTextA(gui_data->hOutputEdit, output_prefix, 256);
    
    char online_filename[256];
    char offline_filename[256];
    
    sprintf(online_filename, "%s.txt", output_prefix);
    sprintf(offline_filename, "offline_links.txt");
    
    FILE* online_file = fopen(online_filename, "w");
    FILE* offline_file = fopen(offline_filename, "w");
    
    if (!online_file || !offline_file) {
        MessageBoxA(hwnd, "Error creating files!", "Error", MB_ICONERROR);
        return;
    }
    
    for (int i = 0; i < gui_data->thread_data->count; i++) {
        if (strstr(gui_data->thread_data->results[i].status, "Online")) {
            fprintf(online_file, "%s\n", gui_data->thread_data->results[i].url);
        } else {
            fprintf(offline_file, "%s # %s\n", gui_data->thread_data->results[i].url, 
                    gui_data->thread_data->results[i].status);
        }
    }
    
    fclose(online_file);
    fclose(offline_file);
    
    char message[512];
    sprintf(message, "Results saved:\nWorking links: %s\nBroken links: %s", 
            online_filename, offline_filename);
    
    MessageBoxA(hwnd, message, "Success", MB_ICONINFORMATION);
}

// Functions from original code
int is_valid_url(const char* url) {
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

char** read_urls_from_file(const char* filename, int* count) {
    FILE* file = fopen(filename, "r");
    if (!file) return NULL;

    char** urls = malloc(MAX_LINES * sizeof(char*));
    char line[MAX_URL_LENGTH];
    *count = 0;

    while (fgets(line, sizeof(line), file) && *count < MAX_LINES) {
        line[strcspn(line, "\r\n")] = 0;
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strlen(trimmed) == 0 || trimmed[0] == '#') continue;
        if (is_valid_url(trimmed)) {
            urls[*count] = malloc(strlen(trimmed) + 1);
            strcpy(urls[*count], trimmed);
            (*count)++;
        }
    }

    fclose(file);
    return urls;
}

CheckResult check_url(const char* url, int timeout_ms) {
    CheckResult result;
    strncpy(result.url, url, MAX_URL_LENGTH);
    result.response_time = 0.0;

    HINTERNET hInternet = InternetOpenA("IPTVChecker/1.0", 
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    
    if (!hInternet) {
        strcpy(result.status, "Offline (InternetOpen failed)");
        return result;
    }

    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    DWORD options = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | 
                   INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_UI;

    clock_t start = clock();
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, NULL, 0, options, 0);
    clock_t end = clock();
    result.response_time = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;

    if (hUrl) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, 
                          &statusCode, &statusSize, NULL)) {
            if (statusCode >= 200 && statusCode < 300) {
                strcpy(result.status, "Online");
            } else {
                sprintf(result.status, "Offline (HTTP %lu)", statusCode);
            }
        } else {
            strcpy(result.status, "Offline (No status code)");
        }
        
        InternetCloseHandle(hUrl);
    } else {
        DWORD error = GetLastError();
        switch (error) {
            case ERROR_INTERNET_TIMEOUT:
                strcpy(result.status, "Offline (Timeout)");
                break;
            case ERROR_INTERNET_NAME_NOT_RESOLVED:
                strcpy(result.status, "Offline (DNS failed)");
                break;
            case ERROR_INTERNET_CANNOT_CONNECT:
                strcpy(result.status, "Offline (Cannot connect)");
                break;
            default:
                sprintf(result.status, "Offline (Error %lu)", error);
        }
    }

    InternetCloseHandle(hInternet);
    return result;
}