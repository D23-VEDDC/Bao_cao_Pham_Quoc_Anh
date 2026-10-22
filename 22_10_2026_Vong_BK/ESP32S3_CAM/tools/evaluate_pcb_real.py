"""Run the firmware PCB detector/normalizer on zipped 640x480 JPEG captures.

Usage: python tools/evaluate_pcb_real.py output_directory archive.zip [...]
Requires Pillow and a host GCC. Diagnostic overlays/crops are saved for review.
"""

import ctypes
import io
import pathlib
import subprocess
import sys
import tempfile
import zipfile

from PIL import Image, ImageDraw


ROOT = pathlib.Path(__file__).resolve().parents[1]


class PcbBox(ctypes.Structure):
    _fields_ = [
        ("corners", (ctypes.c_float * 2) * 4),
        ("angle_deg", ctypes.c_float),
        ("foreground_pixels", ctypes.c_int),
    ]


def compile_detector(temp):
    (temp / "freertos").mkdir()
    (temp / "esp_heap_caps.h").write_text(
        "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n"
        "static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }\n"
        "static inline void *heap_caps_calloc(size_t n, size_t s, int c) { (void)c; return calloc(n, s); }\n"
        "static inline void *heap_caps_free(void *p) { free(p); }\n"
    )
    (temp / "esp_log.h").write_text(
        '#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf(fmt "\\n", ##__VA_ARGS__)\n'
        '#define ESP_LOGW(tag,fmt,...) printf(fmt "\\n", ##__VA_ARGS__)\n'
    )
    (temp / "freertos" / "FreeRTOS.h").write_text("")
    (temp / "freertos" / "task.h").write_text(
        "static inline void vTaskDelay(int n) { (void)n; }\n"
    )
    dll = temp / "pcb_preprocess.dll"
    subprocess.run(
        ["gcc", "-shared", "-O2", "-std=c11", "-I", str(temp),
         "-I", str(ROOT / "main"), str(ROOT / "main" / "pcb_preprocess.c"),
         "-o", str(dll)],
        check=True,
    )
    return dll


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    output = pathlib.Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp_dir:
        dll = compile_detector(pathlib.Path(temp_dir))
        library = ctypes.CDLL(str(dll))
        detector = library.pcb_detect_rgb
        detector.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int,
                             ctypes.c_int, ctypes.POINTER(PcbBox)]
        detector.restype = ctypes.c_bool
        normalizer = library.pcb_normalize_rgb
        normalizer.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int,
                               ctypes.c_int, ctypes.POINTER(PcbBox),
                               ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
        total = failed = 0
        for archive_name in sys.argv[2:]:
            archive = pathlib.Path(archive_name)
            archive_output = output / archive.stem
            archive_output.mkdir(exist_ok=True)
            with zipfile.ZipFile(archive) as zipped:
                entries = [entry for entry in zipped.infolist()
                           if entry.filename.lower().endswith((".jpg", ".jpeg"))]
                for index, entry in enumerate(entries):
                    image = Image.open(io.BytesIO(zipped.read(entry))).convert("RGB")
                    if image.size != (640, 480):
                        raise ValueError((entry.filename, image.size))
                    frame = image.tobytes()
                    pixels = (ctypes.c_uint8 * len(frame)).from_buffer_copy(frame)
                    box = PcbBox()
                    found = detector(pixels, 640, 480, ctypes.byref(box))
                    total += 1
                    if not found:
                        failed += 1
                        print(f"FAIL {archive.name} {index:02} {entry.filename}")
                        continue
                    corners = [(float(p[0]), float(p[1])) for p in box.corners]
                    outline = image.copy()
                    draw = ImageDraw.Draw(outline)
                    draw.line(corners + corners[:1], fill=(255, 0, 0), width=3)
                    outline.save(archive_output / f"{index:02}_box.jpg")
                    crop = (ctypes.c_uint8 * (640 * 448 * 3))()
                    normalizer(pixels, 640, 480, ctypes.byref(box), crop, 640, 448)
                    Image.frombytes("RGB", (640, 448), bytes(crop)).save(
                        archive_output / f"{index:02}_crop.jpg", quality=90
                    )
                    print(f"OK   {archive.name} {index:02} angle={box.angle_deg:5.1f} "
                          f"pixels={box.foreground_pixels:5d} "
                          f"corners={[(round(x), round(y)) for x, y in corners]}")
        print(f"TOTAL {total} FAILED {failed}")
        del normalizer, detector
        free_library = ctypes.windll.kernel32.FreeLibrary
        free_library.argtypes = [ctypes.c_void_p]
        free_library.restype = ctypes.c_int
        assert free_library(library._handle)
        if failed:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
