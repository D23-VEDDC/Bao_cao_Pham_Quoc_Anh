#!/usr/bin/env python3
"""Compare a captured ESP32 RGB patch/vector with the embedded TFLite model.

Save the 12,288-byte RGB dump from the UART debug mode to patch.rgb. Save the
32 normalized [EMBED VECTOR] l2 values for that patch as a JSON array.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import numpy as np

try:
    import tensorflow as tf
    Interpreter = tf.lite.Interpreter
except ImportError:
    try:
        from tflite_runtime.interpreter import Interpreter
    except ImportError as exc:
        raise SystemExit("Install TensorFlow or tflite-runtime, plus numpy.") from exc


def read_vector(path: str | None) -> np.ndarray | None:
    if path is None:
        return None
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    vector = np.asarray(value, dtype=np.float32).reshape(-1)
    if vector.shape != (32,):
        raise SystemExit(f"{path}: expected 32 floats, got {vector.shape}")
    return vector


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="main/pcb_embedding_64_i8in_f32out.tflite")
    ap.add_argument("--patch-rgb", required=True, help="Raw 64x64 RGB888 bytes (12,288 bytes)")
    ap.add_argument("--esp-vector", help="JSON array of ESP32 L2-normalized 32-vector")
    ap.add_argument("--esp-raw", help="JSON array of ESP32 raw 32-float model output")
    ap.add_argument("--save-host", help="Write host raw and normalized vectors to JSON")
    args = ap.parse_args()

    rgb_bytes = Path(args.patch_rgb).read_bytes()
    if len(rgb_bytes) != 64 * 64 * 3:
        raise SystemExit(f"Expected 12,288 RGB bytes, got {len(rgb_bytes)}")
    rgb = np.frombuffer(rgb_bytes, dtype=np.uint8).reshape(1, 64, 64, 3)

    interpreter = Interpreter(model_path=str(Path(args.model).resolve()))
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    print("Model input:", inp["shape"], inp["dtype"], inp["quantization"])
    print("Model output:", out["shape"], out["dtype"])
    if tuple(inp["shape"]) != (1, 64, 64, 3) or inp["dtype"] != np.int8:
        raise SystemExit("Expected model input [1,64,64,3] INT8")
    if tuple(out["shape"]) != (1, 32) or out["dtype"] != np.float32:
        raise SystemExit("Expected model output [1,32] FLOAT32")

    scale, zero_point = inp["quantization"]
    if not np.isfinite(scale) or scale <= 0:
        raise SystemExit(f"Invalid model input scale: {scale}")
    q = np.rint(rgb.astype(np.float32) / scale + zero_point)
    q = np.clip(q, -128, 127).astype(np.int8)
    interpreter.set_tensor(inp["index"], q)
    interpreter.invoke()
    raw = interpreter.get_tensor(out["index"])[0].astype(np.float32)
    if not np.isfinite(raw).all():
        raise SystemExit("Host model returned non-finite output")
    norm = float(np.linalg.norm(raw))
    if norm < 1e-12:
        raise SystemExit("Host model returned a zero vector")
    normalized = raw / norm
    print(f"Host raw norm={norm:.9g} min={raw.min():.9g} max={raw.max():.9g}")
    print("Host raw:", json.dumps(raw.tolist()))
    print("Host L2 :", json.dumps(normalized.tolist()))

    esp = read_vector(args.esp_vector)
    if esp is not None:
        print(f"L2 max_abs={np.max(np.abs(esp-normalized)):.9g} cosine={np.dot(esp, normalized):.9g}")
    esp_raw = read_vector(args.esp_raw)
    if esp_raw is not None:
        print(f"Raw max_abs={np.max(np.abs(esp_raw-raw)):.9g}")
    if args.save_host:
        Path(args.save_host).write_text(json.dumps({"raw": raw.tolist(), "l2": normalized.tolist()}, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
