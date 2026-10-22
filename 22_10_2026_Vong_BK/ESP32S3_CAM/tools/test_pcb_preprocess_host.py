"""Smoke-test the firmware PCB detector with synthetic camera frames on a host PC."""

import ctypes
import math
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


class PcbBox(ctypes.Structure):
    _fields_ = [
        ("corners", (ctypes.c_float * 2) * 4),
        ("angle_deg", ctypes.c_float),
        ("foreground_pixels", ctypes.c_int),
    ]


def make_frame(board_color=None, angle=0, half_w=150, half_h=160,
               center=(320, 240), background=(32, 68, 54)):
    width, height = 640, 480
    frame = bytearray(bytes(background) * (width * height))
    c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    for y in range(height):
        for x in range(width):
            if x < 24 or x >= 616:
                color = (230, 230, 230)
            elif board_color is not None:
                u = (x - center[0]) * c + (y - center[1]) * s
                v = -(x - center[0]) * s + (y - center[1]) * c
                if abs(u) < half_w and abs(v) < half_h:
                    color = board_color
                    if -125 < u < 125 and -135 < v < 135 and (
                        abs((u + 115) % 35 - 17) < 2 or abs((v + 120) % 52 - 26) < 2
                    ):
                        color = (100, 95, 83)
                else:
                    continue
            else:
                continue
            offset = (y * width + x) * 3
            frame[offset : offset + 3] = bytes(color)
    return frame


def make_perspective_yellow_frame():
    width, height = 640, 480
    frame = bytearray(bytes((32, 68, 54)) * (width * height))
    for y in range(90, 390):
        fraction = (y - 90) / 300
        left = round(95 - 15 * fraction)
        right = round(520 + 50 * fraction)
        for x in range(left, right):
            offset = (y * width + x) * 3
            frame[offset : offset + 3] = bytes((165, 159, 130))
    return frame


def main():
    with tempfile.TemporaryDirectory() as temp:
        temp = pathlib.Path(temp)
        (temp / "freertos").mkdir()
        (temp / "esp_heap_caps.h").write_text(
            "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n"
            "static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }\n"
            "static inline void *heap_caps_calloc(size_t n, size_t s, int c) { (void)c; return calloc(n, s); }\n"
            "static inline void heap_caps_free(void *p) { free(p); }\n"
        )
        (temp / "esp_log.h").write_text(
            "#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n"
        )
        (temp / "freertos" / "FreeRTOS.h").write_text("")
        (temp / "freertos" / "task.h").write_text(
            "static inline void vTaskDelay(int n) { (void)n; }\n"
        )
        dll = temp / "pcb_preprocess.dll"
        subprocess.run(
            [
                "gcc", "-shared", "-O2", "-std=c11", "-I", str(temp),
                "-I", str(ROOT / "main"), str(ROOT / "main" / "pcb_preprocess.c"),
                "-o", str(dll),
            ],
            check=True,
        )
        library = ctypes.CDLL(str(dll))
        detector = library.pcb_detect_rgb
        detector.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.POINTER(PcbBox)]
        detector.restype = ctypes.c_bool
        for name, color, angle, half_w, half_h, center, background in [
            ("yellow", (186, 164, 100), 0, 150, 160, (320, 240), (32, 68, 54)),
            ("yellow rotated", (186, 164, 100), 9, 150, 160, (320, 240), (32, 68, 54)),
            ("yellow portrait", (186, 164, 100), 0, 150, 198, (320, 240), (32, 68, 54)),
            ("yellow moved on green mat", (186, 164, 100), 0, 150, 198, (330, 245), (30, 115, 75)),
            ("pale yellow portrait", (165, 159, 130), 0, 150, 198, (320, 240), (32, 68, 54)),
            ("green", (36, 139, 69), 0, 150, 160, (320, 240), (32, 68, 54)),
            ("green rotated", (36, 139, 69), 9, 150, 160, (320, 240), (32, 68, 54)),
            ("green landscape", (36, 139, 69), 0, 250, 170, (320, 240), (32, 68, 54)),
            ("no board", None, 0, 150, 160, (320, 240), (32, 68, 54)),
            ("yellow background only", None, 0, 150, 160, (320, 240), (100, 95, 70)),
        ]:
            frame = make_frame(color, angle, half_w, half_h, center, background)
            pixels = (ctypes.c_uint8 * len(frame)).from_buffer(frame)
            box = PcbBox()
            found = detector(pixels, 640, 480, ctypes.byref(box))
            assert found == (color is not None), (name, found)
            if found:
                center_x = sum(p[0] for p in box.corners) / 4
                center_y = sum(p[1] for p in box.corners) / 4
                assert abs(center_x - center[0]) < 25 and abs(center_y - center[1]) < 25, (
                    name, center_x, center_y
                )
                side_w = math.dist(box.corners[0], box.corners[1])
                side_h = math.dist(box.corners[0], box.corners[3])
                assert abs(side_w - 2 * half_w) < 35 and abs(side_h - 2 * half_h) < 35, (
                    name, side_w, side_h
                )
                if angle:
                    assert abs(box.angle_deg - angle) < 3, (name, box.angle_deg)
                print(f"{name}: center=({center_x:.1f}, {center_y:.1f}), size=({side_w:.1f}, {side_h:.1f}), angle={box.angle_deg:.1f}")
            else:
                print(f"{name}: rejected")
        perspective = make_perspective_yellow_frame()
        pixels = (ctypes.c_uint8 * len(perspective)).from_buffer(perspective)
        box = PcbBox()
        assert detector(pixels, 640, 480, ctypes.byref(box))
        expected = ((95, 90), (520, 90), (570, 390), (80, 390))
        for found, target in zip(box.corners, expected):
            assert math.dist(found, target) < 16, (tuple(found), target)
        print("yellow perspective: all four corners follow the substrate")
        del detector
        free_library = ctypes.windll.kernel32.FreeLibrary
        free_library.argtypes = [ctypes.c_void_p]
        free_library.restype = ctypes.c_int
        assert free_library(library._handle)


if __name__ == "__main__":
    main()
