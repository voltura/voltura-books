"""Exercise only this app's windows. Refuses to overwrite existing user settings."""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import winreg

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "dist/VolturaBooks.exe"
DATA = Path(os.environ["LOCALAPPDATA"]) / "Voltura Books"
INSTALLED = Path(os.environ["LOCALAPPDATA"]) / "Programs/Voltura Books"
KEY = r"Software\Classes\*\shell\VolturaBooks.Send"
user = c.WinDLL("user32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
advapi = c.WinDLL("advapi32", use_last_error=True)
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = [CALLBACK, w.LPARAM]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetDlgItem.argtypes = [w.HWND, c.c_int]
user.GetDlgItem.restype = w.HWND
user.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = c.c_ssize_t
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.IsWindowVisible.argtypes = [w.HWND]
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]


class Credential(c.Structure):
    _fields_ = [("Flags", w.DWORD), ("Type", w.DWORD), ("TargetName", w.LPWSTR),
                ("Comment", w.LPWSTR), ("LastWritten", w.FILETIME), ("BlobSize", w.DWORD),
                ("Blob", c.POINTER(c.c_ubyte)), ("Persist", w.DWORD), ("AttributeCount", w.DWORD),
                ("Attributes", c.c_void_p), ("TargetAlias", w.LPWSTR), ("UserName", w.LPWSTR)]


advapi.CredReadW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.POINTER(c.POINTER(Credential))]
advapi.CredFree.argtypes = [c.c_void_p]


def password():
    pointer = c.POINTER(Credential)()
    if not advapi.CredReadW("VolturaBooks/SMTP", 1, 0, c.byref(pointer)):
        assert c.get_last_error() == 1168
        return None
    try:
        cred = pointer.contents
        return c.string_at(cred.Blob, cred.BlobSize).decode("utf-16-le")
    finally:
        advapi.CredFree(pointer)


def windows(pid):
    found = []
    @CALLBACK
    def each(hwnd, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            title = c.create_unicode_buffer(512)
            user.GetWindowTextW(hwnd, title, len(title))
            found.append((hwnd, title.value))
        return True
    user.EnumWindows(each, 0)
    return found


def wait_window(process, title):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        for hwnd, actual in windows(process.pid):
            if actual == title:
                return hwnd
        if process.poll() is not None:
            raise AssertionError(f"App exited early: {process.returncode}")
        time.sleep(.01)
    raise AssertionError(f"Missing window: {title}; {windows(process.pid)}")


def set_text(hwnd, control, value):
    buffer = c.create_unicode_buffer(value)
    assert user.SendMessageW(user.GetDlgItem(hwnd, control), 0x000C, 0, c.addressof(buffer))


def click(hwnd, control):
    # Post the same WM_COMMAND notification as a button click, without blocking
    # this test when the app opens a modal validation dialog.
    user.PostMessageW(hwnd, 0x111, control, user.GetDlgItem(hwnd, control) or 0)


def memory(pid):
    class Counters(c.Structure):
        _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD)] + [
            (name, c.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
    process = kernel.OpenProcess(0x410, False, pid)
    counters = Counters(); counters.cb = c.sizeof(counters)
    psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.c_void_p, w.DWORD]
    assert psapi.GetProcessMemoryInfo(process, c.byref(counters), counters.cb)
    kernel.CloseHandle(process)
    return counters.PeakWorkingSetSize


def screenshot(hwnd, name):
    # Render this window directly even when it is behind another application.
    from PIL import Image
    user.SetProcessDPIAware()
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    rect = w.RECT()
    assert user.GetWindowRect(hwnd, c.byref(rect))
    target = ROOT / ".cache" / name
    target.parent.mkdir(exist_ok=True)
    width, height = rect.right - rect.left, rect.bottom - rect.top
    gdi = c.WinDLL("gdi32")
    gdi.CreateCompatibleDC.argtypes = [w.HDC]; gdi.CreateCompatibleDC.restype = w.HDC
    gdi.CreateDIBSection.argtypes = [w.HDC, c.c_void_p, w.UINT, c.POINTER(c.c_void_p), w.HANDLE, w.DWORD]
    gdi.CreateDIBSection.restype = w.HANDLE
    gdi.SelectObject.argtypes = [w.HDC, w.HANDLE]; gdi.SelectObject.restype = w.HANDLE
    gdi.DeleteObject.argtypes = [w.HANDLE]; gdi.DeleteDC.argtypes = [w.HDC]
    user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
    import struct
    info = c.create_string_buffer(struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0))
    dc = gdi.CreateCompatibleDC(None)
    pixels = c.c_void_p()
    bitmap = gdi.CreateDIBSection(dc, info, 0, c.byref(pixels), None, 0)
    previous = gdi.SelectObject(dc, bitmap)
    try:
        assert user.PrintWindow(hwnd, dc, 2)
        Image.frombytes('RGB', (width, height), c.string_at(pixels, width * height * 4), 'raw', 'BGRX').save(target)
    finally:
        gdi.SelectObject(dc, previous); gdi.DeleteObject(bitmap); gdi.DeleteDC(dc)


def association():
    path = r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.epub\UserChoice"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            return winreg.QueryValueEx(key, "ProgId")[0]
    except FileNotFoundError:
        return None


def main():
    if DATA.exists() or INSTALLED.exists() or password() is not None:
        raise SystemExit("Refusing smoke test: existing user settings, credentials or installation.")
    initial_association = association()
    launch_times, peak = [], 0
    with tempfile.TemporaryDirectory(prefix="voltura-books-ui-") as temp:
        book = Path(temp) / "Böcker & spaces.epub"
        book.write_bytes(b"PK test fixture")
        # First-run cancellation must not persist settings or send.
        process = subprocess.Popen([str(EXE), "--send", str(book)])
        hwnd = wait_window(process, "Voltura Books - Settings")
        click(hwnd, 2); process.wait(5)
        assert not DATA.exists() and password() is None
        print("PASS first-run cancellation")
        # Invalid settings stay in the dialog. Then save through the real UI.
        process = subprocess.Popen([str(EXE), "--send", str(book)])
        hwnd = wait_window(process, "Voltura Books - Settings")
        time.sleep(.15)
        assert not user.IsWindowVisible(user.GetDlgItem(hwnd, 1005))
        screenshot(hwnd, "settings.png")
        click(hwnd, 1)
        alert = wait_window(process, "Voltura Books"); click(alert, 2)
        set_text(hwnd, 1001, "reader@kindle.com")
        set_text(hwnd, 1002, "sender@example.com")
        set_text(hwnd, 1003, "first-fixture-password")
        user.SendMessageW(user.GetDlgItem(hwnd, 1004), 0xF1, 1, 0)
        click(hwnd, 1004)
        time.sleep(.1)
        assert user.IsWindowVisible(user.GetDlgItem(hwnd, 1005))
        screenshot(hwnd, "settings-advanced.png")
        # Configure an unreachable loopback endpoint; never send externally.
        set_text(hwnd, 1005, "localhost"); set_text(hwnd, 1006, "1")
        click(hwnd, 1)
        send = wait_window(process, "Voltura Books - Send to Kindle")
        # Saving first-run settings must immediately continue the pending send.
        time.sleep(3)
        screenshot(send, "send-error.png")
        click(send, 2); assert process.wait(5) == 1
        assert password() == "first-fixture-password"
        assert "fixture-password" not in (DATA / "settings.ini").read_text(encoding="utf-16")
        print("PASS settings validation and credential storage")
        for secret in ("", "replacement-fixture-password"):
            start = time.perf_counter()
            process = subprocess.Popen([str(EXE), "--settings"])
            hwnd = wait_window(process, "Voltura Books - Settings")
            launch_times.append((time.perf_counter() - start) * 1000)
            peak = max(peak, memory(process.pid))
            set_text(hwnd, 1003, secret); click(hwnd, 1); assert process.wait(5) == 0
            assert password() == (secret or None)
        print("PASS cleared and replaced credentials")
        process = subprocess.Popen([str(EXE), "--send", str(book)])
        hwnd = wait_window(process, "Voltura Books - Send to Kindle")
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            text = c.create_unicode_buffer(1024)
            user.GetWindowTextW(user.GetDlgItem(hwnd, 1011), text, len(text))
            if "Could not reach" in text.value:
                break
            time.sleep(.05)
        assert "Could not reach" in text.value
        peak = max(peak, memory(process.pid))
        click(hwnd, 2); assert process.wait(5) == 1
        print("PASS send error remains visible and process exits")
    assert subprocess.run([str(EXE), "--install"], timeout=10).returncode == 0
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY + r"\command") as key:
        command = winreg.QueryValueEx(key, None)[0]
        assert command == f'"{INSTALLED / "VolturaBooks.exe"}" --send "%1"'
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as key:
        assert winreg.QueryValueEx(key, "MultiSelectModel")[0] == "Player"
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY + r"\DropTarget") as key:
        assert winreg.QueryValueEx(key, "CLSID")[0] == "{F0D73B4A-36AE-40A5-A224-CFD826524030}"
    assert association() == initial_association
    assert (Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/Voltura Books - Settings.lnk").exists()
    assert (Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/Voltura Books - Send a book.lnk").exists()
    print("PASS per-user installation, quoted context command and unchanged default reader")
    # Invoke the registered Explorer verb through Windows Shell, not a direct CLI.
    class ShellInfo(c.Structure):
        _fields_ = [("cbSize", w.DWORD), ("fMask", w.ULONG), ("hwnd", w.HWND),
                    ("lpVerb", w.LPCWSTR), ("lpFile", w.LPCWSTR), ("lpParameters", w.LPCWSTR),
                    ("lpDirectory", w.LPCWSTR), ("nShow", c.c_int), ("hInstApp", w.HANDLE),
                    ("lpIDList", c.c_void_p), ("lpClass", w.LPCWSTR), ("hkeyClass", w.HANDLE),
                    ("dwHotKey", w.DWORD), ("hIcon", w.HANDLE), ("hProcess", w.HANDLE)]
    shell = c.WinDLL("shell32")
    shell.ShellExecuteExW.argtypes = [c.POINTER(ShellInfo)]
    kernel.GetProcessId.argtypes = [w.HANDLE]; kernel.GetProcessId.restype = w.DWORD
    kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    with tempfile.TemporaryDirectory(prefix="voltura-books-context-") as temp:
        book = Path(temp) / "Böcker & spaces.epub"; book.write_bytes(b"PK fixture")
        info = ShellInfo(); info.cbSize = c.sizeof(info); info.fMask = 0x40 | 0x100
        info.lpVerb = "VolturaBooks.Send"; info.lpFile = str(book); info.nShow = 1
        assert shell.ShellExecuteExW(c.byref(info))
        class ShellProcess:
            pid = kernel.GetProcessId(info.hProcess)
            returncode = None
            def poll(self):
                return 0 if kernel.WaitForSingleObject(info.hProcess, 0) == 0 else None
        hwnd = wait_window(ShellProcess(), "Voltura Books - Send to Kindle")
        time.sleep(3)
        click(hwnd, 2)
        assert kernel.WaitForSingleObject(info.hProcess, 10000) == 0
        kernel.CloseHandle(info.hProcess)
    print("PASS Explorer shell verb launches the installed sender for a Unicode filename")
    assert subprocess.run([str(INSTALLED / "VolturaBooks.exe"), "--uninstall"], timeout=10).returncode == 0
    deadline = time.monotonic() + 15
    while INSTALLED.exists() and time.monotonic() < deadline:
        time.sleep(.1)
    assert not INSTALLED.exists() and not DATA.exists() and password() is None
    try:
        winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY)
        raise AssertionError("Context registration survived uninstall")
    except FileNotFoundError:
        pass
    assert association() == initial_association
    print("PASS uninstall removes package, settings, credentials and registration")
    metrics = {"settings_launch_ms": launch_times, "peak_working_set_mib": round(peak / 1024**2, 2),
               "executable_bytes": EXE.stat().st_size}
    (ROOT / "build/smoke-metrics.json").write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics))


if __name__ == "__main__":
    main()
