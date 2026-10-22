"""Replacement Colab export cell for a Keras 64x64 RGB embedding model.

Set `keras_model` to the trained Keras model and put actual 640x448 normalized
BMP captures from `/api/normalized-image` in FRAME_DIR. Include expected
bright and dim conditions and both clean/defective boards.
Run this in Colab with TensorFlow, Pillow, and NumPy installed. The model
itself is not present in this repository, so this cell cannot re-export it
without the training artifact.
"""

from pathlib import Path

import numpy as np
from PIL import Image
import tensorflow as tf

FRAME_DIR = Path("/content/normalized_pcb_frames")
OUTPUT_PATH = Path("/content/pcb_embedding_64_i8in_f32out.tflite")
PATCH = 64
STRIDE = 48
MAX_COLS = 13
MAX_ROWS = 9


def firmware_patches(frame_path):
    """Mirror the firmware's row-major overlapping patches on normalized RGB."""
    with Image.open(frame_path) as image:
        frame = np.asarray(image.convert("RGB"), dtype=np.uint8)
    height, width, channels = frame.shape
    if (height, width, channels) != (448, 640, 3):
        raise ValueError(f"{frame_path}: expected 640x448 normalized RGB, got {width}x{height}")
    for row in range(MAX_ROWS):
        for col in range(MAX_COLS):
            x, y = col * STRIDE, row * STRIDE
            yield np.ascontiguousarray(frame[y:y + PATCH, x:x + PATCH])


frame_paths = sorted(p for p in FRAME_DIR.iterdir()
                     if p.suffix.lower() in {".bmp", ".png"})
if not frame_paths:
    raise ValueError(f"No real webcam frames in {FRAME_DIR}")


def representative_dataset():
    for frame_path in frame_paths:
        for patch_rgb in firmware_patches(frame_path):
            # The model contains Rescaling(1/127.5, offset=-1): feed raw
            # RGB 0..255 here, exactly as the firmware does before INT8 quantization.
            yield [patch_rgb.astype(np.float32)[None, ...]]


assert tuple(keras_model.input_shape[1:]) == (64, 64, 3)
converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.float32
OUTPUT_PATH.write_bytes(converter.convert())
print(f"Exported {OUTPUT_PATH} from {len(frame_paths)} normalized PCB frames, "
      f"{len(frame_paths) * MAX_COLS * MAX_ROWS} RGB patches")
