"""Capture d'une fenetre par duplication de bureau (bibliotheque dxcam, PyPI).

Les captures GDI / PrintWindow de la fenetre Ryujinx donnent une zone de jeu
noire (constate le 2026-11-10/11). La duplication de bureau recupere l'image
finale composee envoyee a l'ecran.

Usage : python tools/capture_dd.py <pid> <sortie.png> [largeur_max]
La fenetre doit etre visible a l'ecran (elle est amenee au premier plan).
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time


def find_main_window(pid):
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def callback(hwnd, _):
        wpid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(hwnd) and user32.GetWindowTextLengthW(hwnd) > 0:
            found.append(hwnd)
        return True

    user32.EnumWindows(callback, 0)
    return found[0] if found else None


def main():
    pid = int(sys.argv[1])
    out = sys.argv[2]
    max_width = int(sys.argv[3]) if len(sys.argv) > 3 else 960

    ctypes.windll.user32.SetProcessDPIAware()
    hwnd = find_main_window(pid)
    if not hwnd:
        print("pas de fenetre")
        return 1

    user32 = ctypes.windll.user32
    # La fenetre passe au-dessus des autres SANS prendre le clavier : l'utilisateur
    # peut jouer a autre chose sur l'autre ecran pendant les essais.
    SWP = 0x0001 | 0x0002 | 0x0010  # NOSIZE | NOMOVE | NOACTIVATE
    user32.SetWindowPos(wt.HWND(hwnd), wt.HWND(-1), 0, 0, 0, 0, SWP)
    time.sleep(0.5)

    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))

    # Ecran qui porte la fenetre, et sa position sur le bureau.
    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]

    user32.MonitorFromWindow.restype = wt.HMONITOR
    mon = user32.MonitorFromWindow(wt.HWND(hwnd), 2)
    info = MONITORINFO()
    info.cbSize = ctypes.sizeof(MONITORINFO)
    user32.GetMonitorInfoW(wt.HMONITOR(mon), ctypes.byref(info))
    mx, my = info.rcMonitor.left, info.rcMonitor.top
    msize = (info.rcMonitor.right - mx, info.rcMonitor.bottom - my)
    primary = bool(info.dwFlags & 1)

    import dxcam
    from PIL import Image

    # Sortie dxcam correspondante : reperee par sa taille et son statut d'ecran principal.
    idx = 0
    for line in dxcam.output_info().splitlines():
        if f"Res:{msize}" in line and f"Primary:{primary}" in line:
            idx = int(line.split("Output[")[1].split("]")[0])
            break

    # BGRA natif : la conversion de couleur de dxcam demande OpenCV, on la fait nous-memes.
    cam = dxcam.create(output_idx=idx, output_color="BGRA")
    sw, sh = cam.width, cam.height
    region = (max(0, rect.left - mx), max(0, rect.top - my), min(sw, rect.right - mx), min(sh, rect.bottom - my))
    frame = None
    for _ in range(20):
        frame = cam.grab(region=region)
        if frame is not None:
            break
        time.sleep(0.1)
    user32.SetWindowPos(wt.HWND(hwnd), wt.HWND(-2), 0, 0, 0, 0, SWP)  # plus "toujours au-dessus"
    if frame is None:
        print("aucune image obtenue")
        return 1

    img = Image.fromarray(frame[:, :, [2, 1, 0]])
    if img.width > max_width:
        img = img.resize((max_width, img.height * max_width // img.width))
    img.save(out)
    # Indication rapide : proportion de pixels non noirs dans la zone centrale.
    cx = img.crop((img.width // 8, img.height // 6, img.width * 7 // 8, img.height * 5 // 6)).convert("L")
    hist = cx.histogram()
    non_black = 1.0 - sum(hist[:12]) / float(cx.width * cx.height)
    print(f"{out} ({img.width}x{img.height}), pixels non noirs au centre : {non_black * 100:.1f} %")
    return 0


if __name__ == "__main__":
    sys.exit(main())
