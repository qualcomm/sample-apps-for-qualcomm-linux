# -----------------------------------------------------------------------------
#
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause
#
# -----------------------------------------------------------------------------
#!/usr/bin/env python3
import cv2
import numpy as np
import argparse
import ai_edge_litert.interpreter as tflite
import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst

# -------------------- CLI Options --------------------
parser = argparse.ArgumentParser(description="Run object detection and output to file or Wayland.")
parser.add_argument("--output", choices=["file", "wayland"], default="file",
                    help="Choose output mode: 'file' (default) or 'wayland'")
args = parser.parse_args()

# -------------------- Parameters --------------------
MODEL_PATH = "/etc/models/yolox_quantized.tflite"
LABEL_PATH = "/etc/labels/coco_labels.txt"
VIDEO_IN = "/etc/media/video.mp4"
VIDEO_OUT = "output_object_detection.mp4"
DELEGATE_PATH = "libQnnTFLiteDelegate.so"

FRAME_W, FRAME_H = 1080, 720
FPS_OUT = 30
CONF_THRES = 0.60
NMS_IOU_THRES = 0.50
BOX_SCALE = 3.6124823093414307
BOX_ZP = 38.0
SCORE_SCALE = 0.003626860911026597

# -------------------- Load Model --------------------
delegate_options = {'backend_type': 'htp'}
delegate = tflite.load_delegate(DELEGATE_PATH, delegate_options)
interpreter = tflite.Interpreter(model_path=MODEL_PATH, experimental_delegates=[delegate])
interpreter.allocate_tensors()

in_det = interpreter.get_input_details()
out_det = interpreter.get_output_details()
in_h, in_w = in_det[0]["shape"][1:3]

labels = [l.strip() for l in open(LABEL_PATH)]

cap = cv2.VideoCapture(VIDEO_IN)
sx, sy = FRAME_W / in_w, FRAME_H / in_h
frame_rs = np.empty((FRAME_H, FRAME_W, 3), np.uint8)
input_tensor = np.empty((1, in_h, in_w, 3), np.uint8)

# -------------------- Output Setup --------------------
if args.output == "file":
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out_writer = cv2.VideoWriter(VIDEO_OUT, fourcc, FPS_OUT, (FRAME_W, FRAME_H))
else:
    Gst.init(None)
    pipeline = Gst.parse_launch(
        'appsrc name=src is-live=true block=true format=time caps=video/x-raw,format=BGR,width=1080,height=720,framerate=30/1 ! videoconvert ! waylandsink sync=false'
    )
    appsrc = pipeline.get_by_name('src')
    pipeline.set_state(Gst.State.PLAYING)

frame_cnt = 0

# -------------------- Main Loop --------------------
while True:
    ok, frame = cap.read()
    if not ok:
        break
    frame_cnt += 1

    cv2.resize(frame, (FRAME_W, FRAME_H), dst=frame_rs)
    cv2.resize(frame_rs, (in_w, in_h), dst=input_tensor[0])

    interpreter.set_tensor(in_det[0]['index'], input_tensor)
    interpreter.invoke()

    boxes_q = interpreter.get_tensor(out_det[0]['index'])[0]
    scores_q = interpreter.get_tensor(out_det[1]['index'])[0]
    classes_q = interpreter.get_tensor(out_det[2]['index'])[0]

    boxes = BOX_SCALE * (boxes_q.astype(np.float32) - BOX_ZP)
    scores = SCORE_SCALE * scores_q.astype(np.float32)
    classes = classes_q.astype(np.int32)

    mask = scores >= CONF_THRES
    if np.any(mask):
        boxes_f = boxes[mask]
        scores_f = scores[mask]
        classes_f = classes[mask]

        x1, y1, x2, y2 = boxes_f.T
        boxes_cv2 = np.column_stack((x1, y1, x2 - x1, y2 - y1))

        idx_cv2 = cv2.dnn.NMSBoxes(
            bboxes=boxes_cv2.tolist(),
            scores=scores_f.tolist(),
            score_threshold=CONF_THRES,
            nms_threshold=NMS_IOU_THRES
        )

        if len(idx_cv2):
            idx = idx_cv2.flatten()
            sel_boxes = boxes_f[idx]
            sel_scores = scores_f[idx]
            sel_classes = classes_f[idx]

            sel_boxes[:, [0, 2]] *= sx
            sel_boxes[:, [1, 3]] *= sy
            sel_boxes = sel_boxes.astype(np.int32)

            sel_boxes[:, [0, 2]] = np.clip(sel_boxes[:, [0, 2]], 0, FRAME_W - 1)
            sel_boxes[:, [1, 3]] = np.clip(sel_boxes[:, [1, 3]], 0, FRAME_H - 1)

            for (x1i, y1i, x2i, y2i), sc, cl in zip(sel_boxes, sel_scores, sel_classes):
                cv2.rectangle(frame_rs, (x1i, y1i), (x2i, y2i), (0, 255, 0), 2)
                lab = labels[cl] if cl < len(labels) else str(cl)
                cv2.putText(frame_rs, f"{lab} {sc:.2f}", (x1i, max(10, y1i - 5)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

    # Output based on mode
    if args.output == "file":
        out_writer.write(frame_rs)
    else:
        data = frame_rs.tobytes()
        buf = Gst.Buffer.new_allocate(None, len(data), None)
        buf.fill(0, data)
        buf.duration = Gst.util_uint64_scale_int(1, Gst.SECOND, FPS_OUT)
        timestamp = cap.get(cv2.CAP_PROP_POS_MSEC) * Gst.MSECOND
        buf.pts = buf.dts = int(timestamp)
        appsrc.emit('push-buffer', buf)

# -------------------- Finish --------------------
cap.release()
if args.output == "file":
    out_writer.release()
    print(f"Done – processed video saved to {VIDEO_OUT}")
else:
    appsrc.emit('end-of-stream')
    pipeline.set_state(Gst.State.NULL)
    print("Done – video streamed to Wayland sink")



