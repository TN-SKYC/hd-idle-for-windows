#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#undef UNICODE
#undef _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <vector>
#include <string>
#include <stdio.h>
#include <stddef.h>
#include <mutex>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#include "resource.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef IOCTL_DISK_GET_PERFORMANCE
#define IOCTL_DISK_GET_PERFORMANCE CTL_CODE(IOCTL_DISK_BASE, 0x0008, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#define WM_TRAYICON      (WM_USER + 1)
#define ID_TIMER_UPDATE  2001
#define ID_BTN_SLEEPNOW  2002
#define IDM_OPEN         3001
#define IDM_EXIT         3002

struct DriveInfo {
    std::string model;
    std::string partitions;
    std::string hwStatus;
    HANDLE handle;
    ULONGLONG last_access;
    bool is_sleeping;
    bool isSSD;
    LONGLONG last_io_count;
    int diskIdx;
};

// --- Globals ---
std::vector<DriveInfo*> g_drives;
std::mutex g_driveMutex;

int g_idle_limit = 60;
HWND g_hwndList = NULL;
HWND g_hwndBtn = NULL;
NOTIFYICONDATA nid = { 0 };
int g_sortCol = 3;
bool g_sortAsc = true;

// --- Logic Helpers ---

LONGLONG GetTotalIOCount(HANDLE h) {
    DISK_PERFORMANCE dp = { 0 }; DWORD b;
    if (DeviceIoControl(h, IOCTL_DISK_GET_PERFORMANCE, NULL, 0, &dp, sizeof(dp), &b, NULL))
        return dp.BytesRead.QuadPart + dp.BytesWritten.QuadPart;
    return -1;
}

bool IsSSD(HANDLE h) {
    DEVICE_SEEK_PENALTY_DESCRIPTOR dsp = { 0 };
    STORAGE_PROPERTY_QUERY q = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery };
    DWORD b;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &dsp, sizeof(dsp), &b, NULL)) {
        return !dsp.IncursSeekPenalty;
    }
    return false;
}

std::string CheckAtaPowerMode(HANDLE h, int diskIdx) {
    struct {
        SCSI_PASS_THROUGH_DIRECT sptd;
        BYTE senseData[32];
    } buffer;

    ZeroMemory(&buffer, sizeof(buffer));
    buffer.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    buffer.sptd.CdbLength = 12;
    buffer.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    buffer.sptd.SenseInfoOffset = offsetof(std::remove_reference<decltype(buffer)>::type, senseData);
    buffer.sptd.SenseInfoLength = sizeof(buffer.senseData);
    buffer.sptd.TimeOutValue = 2;

    buffer.sptd.Cdb[0] = 0x85;
    buffer.sptd.Cdb[1] = (3 << 1);
    buffer.sptd.Cdb[2] = 0x20;
    buffer.sptd.Cdb[11] = 0xE5;

    DWORD b;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &buffer, sizeof(buffer), &buffer, sizeof(buffer), &b, NULL))
        return "ERROR_NOT_SUPPORTED";

    BYTE sectorCount = 0;
    bool found = false;

    if (buffer.senseData[0] == 0x72 && buffer.senseData[8] == 0x09) {
        sectorCount = buffer.senseData[13];
        found = true;
    }
    else if (buffer.sptd.Cdb[6] != 0) {
        sectorCount = buffer.sptd.Cdb[6];
        found = true;
    }

    if (!found) return "Active";
    if (sectorCount == 0x00) return "Standby";
    if (sectorCount == 0x80) return "Idle (Spinning)";
    return "Active";
}

void SpinDown(DriveInfo* d) {
    if (!d || d->isSSD) return;

    // Do the slow I/O without holding the lock! 
    // This prevents the UI from freezing.
    SCSI_PASS_THROUGH_DIRECT sptd = { sizeof(SCSI_PASS_THROUGH_DIRECT) };
    sptd.CdbLength = 6; sptd.DataIn = SCSI_IOCTL_DATA_OUT; sptd.TimeOutValue = 5;
    sptd.Cdb[0] = 0x1B; sptd.Cdb[4] = 0x00;
    DWORD b; DeviceIoControl(d->handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd, sizeof(sptd), &sptd, sizeof(sptd), &b, NULL);

    // Now lock just to update the state
    std::lock_guard<std::mutex> lock(g_driveMutex);
    d->is_sleeping = true;
}

std::string GetDriveLetters(DWORD diskNo) {
    std::string res = ""; char drv[256];
    if (GetLogicalDriveStrings(sizeof(drv), drv)) {
        char* d = drv;
        while (*d) {
            char path[12]; sprintf(path, "\\\\.\\%c:", d[0]);
            HANDLE hV = CreateFile(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (hV != INVALID_HANDLE_VALUE) {
                VOLUME_DISK_EXTENTS ex; DWORD b;
                if (DeviceIoControl(hV, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0, 0, &ex, sizeof(ex), &b, 0)) {
                    if (ex.Extents[0].DiskNumber == diskNo) {
                        if (!res.empty()) res += ", ";
                        res += std::string(1, d[0]) + ":";
                    }
                }
                CloseHandle(hV);
            }
            d += strlen(d) + 1;
        }
    }
    return res.empty() ? "-" : res;
}

int CALLBACK CompareDrives(LPARAM lp1, LPARAM lp2, LPARAM lpSort) {
    DriveInfo* d1 = (DriveInfo*)lp1;
    DriveInfo* d2 = (DriveInfo*)lp2;
    std::lock_guard<std::mutex> lock(g_driveMutex);

    int res = 0;
    switch (lpSort) {
    case 0: res = _stricmp(d1->model.c_str(), d2->model.c_str()); break;
    case 1: res = (int)d1->is_sleeping - (int)d2->is_sleeping; break;
    case 2: res = (int)(d1->last_access - d2->last_access); break;
    case 3: res = _stricmp(d1->partitions.c_str(), d2->partitions.c_str()); break;
    }
    return g_sortAsc ? res : -res;
}

void UpdateUI() {
    int selIdx = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);

    std::lock_guard<std::mutex> lock(g_driveMutex);

    for (int i = 0; i < ListView_GetItemCount(g_hwndList); i++) {
        LVITEM lvi = { 0 }; lvi.mask = LVIF_PARAM; lvi.iItem = i;
        ListView_GetItem(g_hwndList, &lvi);
        DriveInfo* d = (DriveInfo*)lvi.lParam;
        if (!d) continue;

        if (d->isSSD) {
            ListView_SetItemText(g_hwndList, i, 1, "N/A");
            ListView_SetItemText(g_hwndList, i, 2, "N/A");
            ListView_SetItemText(g_hwndList, i, 4, "SSD - Unsupported");
        }
        else {
            int rem = max(0, g_idle_limit - (int)((GetTickCount64() - d->last_access) / 1000));
            ListView_SetItemText(g_hwndList, i, 1, (LPSTR)(d->is_sleeping ? "Asleep" : "Active"));
            char buf[32]; sprintf(buf, "%ds", d->is_sleeping ? 0 : rem);
            ListView_SetItemText(g_hwndList, i, 2, buf);
            ListView_SetItemText(g_hwndList, i, 4, (LPSTR)d->hwStatus.c_str());
        }
        if (i == selIdx) EnableWindow(g_hwndBtn, !d->isSSD);
    }
}

// --- Window Proc ---

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwndList = CreateWindowEx(0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER, 10, 10, 100, 100, hWnd, NULL, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hwndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        const char* c[] = { "Model", "Status", "Timer", "Partitions", "Hardware Check" };
        for (int i = 0; i < 5; i++) {
            LVCOLUMN col = { LVCF_TEXT | LVCF_WIDTH, 0, 80, (LPSTR)c[i] };
            ListView_InsertColumn(g_hwndList, i, &col);
        }

        {
            std::lock_guard<std::mutex> lock(g_driveMutex);
            for (auto d : g_drives) {
                LVITEM itm = { LVIF_TEXT | LVIF_PARAM, (int)ListView_GetItemCount(g_hwndList), 0 };
                itm.pszText = (LPSTR)d->model.c_str(); itm.lParam = (LPARAM)d;
                int idx = ListView_InsertItem(g_hwndList, &itm);
                ListView_SetItemText(g_hwndList, idx, 3, (LPSTR)d->partitions.c_str());
            }
        }

        int totalW = 0;
        for (int i = 0; i < 5; i++) {
            ListView_SetColumnWidth(g_hwndList, i, LVSCW_AUTOSIZE);
            int cw = ListView_GetColumnWidth(g_hwndList, i);
            ListView_SetColumnWidth(g_hwndList, i, LVSCW_AUTOSIZE_USEHEADER);
            int hw = ListView_GetColumnWidth(g_hwndList, i);
            int final = (cw > hw ? cw : hw) + 20;
            ListView_SetColumnWidth(g_hwndList, i, final);
            totalW += final;
        }
        HWND hHdr = ListView_GetHeader(g_hwndList);
        RECT rcH; GetWindowRect(hHdr, &rcH);
        RECT rcI; ListView_GetItemRect(g_hwndList, 0, &rcI, LVIR_BOUNDS);
        int listH = (rcH.bottom - rcH.top) + ((int)g_drives.size() * (rcI.bottom - rcI.top)) + 4;

        MoveWindow(g_hwndList, 10, 10, totalW + 4, listH, TRUE);
        g_hwndBtn = CreateWindow("BUTTON", "Force Sleep", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10 + listH + 10, 110, 32, hWnd, (HMENU)ID_BTN_SLEEPNOW, NULL, NULL);

        RECT rcW = { 0, 0, totalW + 24, listH + 62 };
        AdjustWindowRect(&rcW, (DWORD)(WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX), FALSE);
        SetWindowPos(hWnd, NULL, 0, 0, rcW.right - rcW.left, rcW.bottom - rcW.top, SWP_NOMOVE | SWP_NOZORDER);

        ListView_SortItems(g_hwndList, CompareDrives, g_sortCol);
        SetTimer(hWnd, ID_TIMER_UPDATE, 1000, NULL);
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->hwndFrom == g_hwndList && nm->code == LVN_COLUMNCLICK) {
            LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lp;
            if (nmlv->iSubItem == g_sortCol) g_sortAsc = !g_sortAsc;
            else { g_sortCol = nmlv->iSubItem; g_sortAsc = true; }
            ListView_SortItems(g_hwndList, CompareDrives, g_sortCol);
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_SLEEPNOW) {
            int s = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
            if (s != -1) {
                LVITEM lvi = { 0 }; lvi.mask = LVIF_PARAM; lvi.iItem = s;
                ListView_GetItem(g_hwndList, &lvi);
                SpinDown((DriveInfo*)lvi.lParam);
            }
        }
        break;
    case WM_TIMER: UpdateUI(); break;
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) { ShowWindow(hWnd, SW_SHOW); SetForegroundWindow(hWnd); }
        else if (lp == WM_RBUTTONUP) {
            POINT p; GetCursorPos(&p); HMENU m = CreatePopupMenu();
            AppendMenu(m, MF_STRING, 1, "Open"); AppendMenu(m, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(hWnd);
            int s = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hWnd, NULL);
            if (s == 1) ShowWindow(hWnd, SW_SHOW); else if (s == IDM_EXIT) DestroyWindow(hWnd);
            DestroyMenu(m);
        }
        break;
    case WM_CLOSE: ShowWindow(hWnd, SW_HIDE); return 0;
    case WM_DESTROY: {
        KillTimer(hWnd, ID_TIMER_UPDATE);
        Shell_NotifyIcon(NIM_DELETE, &nid);

        std::lock_guard<std::mutex> lock(g_driveMutex);
        for (auto d : g_drives) {
            if (d->handle != INVALID_HANDLE_VALUE) CloseHandle(d->handle);
            delete d;
        }
        g_drives.clear();

        PostQuitMessage(0);
        break;
    }
    default: return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

DWORD WINAPI MonitorThread(LPVOID) {
    {
        std::lock_guard<std::mutex> lock(g_driveMutex);
        for (auto d : g_drives) d->last_io_count = GetTotalIOCount(d->handle);
    }

    while (true) {
        Sleep(6000);

        for (size_t i = 0; i < g_drives.size(); i++) {
            DriveInfo* d = g_drives[i];
            if (d->isSSD) continue;

            // Slow hardware queries happen OUTSIDE the lock
            std::string currentStatus = CheckAtaPowerMode(d->handle, d->diskIdx);
            LONGLONG currentIO = GetTotalIOCount(d->handle);

            bool needsSleep = false;

            // Scope block: Briefly lock to update shared variables
            {
                std::lock_guard<std::mutex> lock(g_driveMutex);
                d->hwStatus = currentStatus;

                if (currentIO > d->last_io_count && currentIO != -1) {
                    d->is_sleeping = false;
                    d->last_access = GetTickCount64();
                    d->last_io_count = currentIO;
                }
                else if (!d->is_sleeping && (GetTickCount64() - d->last_access) / 1000 >= (DWORD)g_idle_limit) {
                    needsSleep = true;
                }
            } // Lock releases here

            // Call SpinDown OUTSIDE the lock to prevent the double-lock crash
            if (needsSleep) {
                SpinDown(d);
            }
        }
    }
    return 0;
}

int APIENTRY WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    InitCommonControls();
    for (int i = 0; i < 16; i++) {
        char p[64]; sprintf(p, "\\\\.\\PhysicalDrive%d", i);
        HANDLE handle = CreateFile(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery }; char b[1024] = { 0 }; DWORD r;
            std::string model = "Disk " + std::to_string(i);
            if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), b, sizeof(b) - 1, &r, 0)) {
                STORAGE_DEVICE_DESCRIPTOR* sd = (STORAGE_DEVICE_DESCRIPTOR*)b;
                if (sd->ProductIdOffset) model = (b + sd->ProductIdOffset);
            }
            bool ssd = IsSSD(handle);
            std::string status = ssd ? "SSD - Unsupported" : CheckAtaPowerMode(handle, i);
            g_drives.push_back(new DriveInfo{ model, GetDriveLetters(i), status, handle, GetTickCount64(), (status == "Standby"), ssd, GetTotalIOCount(handle), i });
        }
    }

    for (auto d : g_drives) {
        std::string res = ""; char drv[256];
        if (GetLogicalDriveStrings(sizeof(drv), drv)) {
            char* ptr = drv;
            while (*ptr) {
                char path[12]; sprintf(path, "\\\\.\\%c:", ptr[0]);
                HANDLE hV = CreateFile(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
                if (hV != INVALID_HANDLE_VALUE) {
                    VOLUME_DISK_EXTENTS ex; DWORD b;
                    if (DeviceIoControl(hV, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0, 0, &ex, sizeof(ex), &b, 0))
                        if (ex.Extents[0].DiskNumber == d->diskIdx) { if (!res.empty()) res += ", "; res += std::string(1, ptr[0]) + ":"; }
                    CloseHandle(hV);
                }
                ptr += strlen(ptr) + 1;
            }
        }
        d->partitions = res.empty() ? "-" : res;
    }

    WNDCLASS wc = { 0 }; wc.lpfnWndProc = WndProc; wc.hInstance = h; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HDIDLE_V25"; RegisterClass(&wc);
    HWND hWnd = CreateWindow("HDIDLE_V25", "HD-Idle Dashboard", (DWORD)(WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX), CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, 0, 0, h, 0);
    nid.cbSize = sizeof(nid); nid.hWnd = hWnd; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON; nid.hIcon = LoadIcon(h, MAKEINTRESOURCE(IDI_ICON2));
    if (!nid.hIcon) nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    Shell_NotifyIcon(NIM_ADD, &nid);
    CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}