"""Range les fenetres de test sur un ecran donne, sans leur donner le clavier.

Usage : python tools/place_windows.py <numero d'ecran, 1 = principal> <pid> [pid...]
Les fenetres sont posees cote a cote en haut de l'ecran choisi. Attend jusqu'a
60 s que chaque fenetre existe.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32


def monitors():
    rects = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)
    def cb(_mon, _dc, rect, _):
        r = rect.contents
        rects.append((r.left, r.top, r.right, r.bottom))
        return True

    user32.EnumDisplayMonitors(None, None, cb, 0)
    # Ecran principal (origine 0,0) en premier, puis de gauche a droite.
    rects.sort(key=lambda r: ((r[0], r[1]) != (0, 0), r[0], r[1]))
    return rects


def main_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        wpid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(hwnd) and user32.GetWindowTextLengthW(hwnd) > 0:
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def main():
    user32.SetProcessDPIAware()
    screen = int(sys.argv[1])
    pids = [int(a) for a in sys.argv[2:]]
    mons = monitors()
    if screen > len(mons):
        print(f"ecran {screen} absent ({len(mons)} ecran(s)) : fenetres laissees en place")
        return 1
    left, top, right, bottom = mons[screen - 1]
    width = (right - left) // max(2, len(pids))
    height = min(bottom - top, width * 9 // 16 + 90)
    for i, pid in enumerate(pids):
        hwnd = None
        for _ in range(120):
            hwnd = main_window(pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print(f"pid {pid} : pas de fenetre")
            continue
        user32.ShowWindow(wt.HWND(hwnd), 4)  # SW_SHOWNOACTIVATE (sort d'un etat maximise)
        # SWP_NOZORDER | SWP_NOACTIVATE
        user32.SetWindowPos(wt.HWND(hwnd), None, left + i * width, top, width, height, 0x0004 | 0x0010)
        print(f"pid {pid} : fenetre posee sur l'ecran {screen} en ({left + i * width}, {top}), {width}x{height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
