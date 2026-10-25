"""Local MediaPipe Tasks models over robot JPEGs, with an annotated preview."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import time
import hashlib

import cv2
import mediapipe as mp
import numpy as np

from control import Face, choose_gesture

HAND_EDGES = ((0,1),(1,2),(2,3),(3,4),(0,5),(5,6),(6,7),(7,8),
              (5,9),(9,10),(10,11),(11,12),(9,13),(13,14),(14,15),
              (15,16),(13,17),(0,17),(17,18),(18,19),(19,20))


@dataclass
class Observation:
    faces: list[Face]
    gesture: str | None
    score: float
    frame: np.ndarray
    inference_ms: float
    brightness: float = 0.0
    hand_points: tuple = ()
    frame_signature: str | None = None
    face_scores: tuple = ()


class Perception:
    def __init__(self, model_dir: Path) -> None:
        vision = mp.tasks.vision
        self.gesture = vision.GestureRecognizer.create_from_options(
            vision.GestureRecognizerOptions(
                base_options=mp.tasks.BaseOptions(model_asset_path=str(model_dir/'gesture_recognizer.task')),
                running_mode=vision.RunningMode.VIDEO, num_hands=2,
                min_hand_detection_confidence=.5, min_hand_presence_confidence=.5))
        try:
            self.face = vision.FaceDetector.create_from_options(vision.FaceDetectorOptions(
                base_options=mp.tasks.BaseOptions(model_asset_path=str(model_dir/'blaze_face_short_range.tflite')),
                running_mode=vision.RunningMode.VIDEO, min_detection_confidence=.55))
        except Exception:
            self.gesture.close()
            raise
        self._timestamp = 0

    def analyze(self, jpeg: bytes) -> Observation:
        started = time.perf_counter()
        bgr = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            raise ValueError('Camera returned an invalid JPEG')
        # Camera pixels stay unmirrored so the motor direction matches sensor coordinates.
        height, width = bgr.shape[:2]
        brightness = float(cv2.cvtColor(bgr,cv2.COLOR_BGR2GRAY).mean())
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        self._timestamp = max(self._timestamp+1, int(time.monotonic()*1000))
        faces = self.face.detect_for_video(image, self._timestamp)
        hands = self.gesture.recognize_for_video(image, self._timestamp)
        boxes = []
        for detection in faces.detections:
            box = detection.bounding_box
            face = Face(box.origin_x/width, box.origin_y/height, box.width/width, box.height/height)
            boxes.append(face)
            cv2.rectangle(bgr, (box.origin_x,box.origin_y),
                          (box.origin_x+box.width,box.origin_y+box.height), (90,210,90), 2)
        candidates = []
        for categories, landmarks in zip(hands.gestures, hands.hand_landmarks):
            if categories:
                category = categories[0]
                candidates.append((category.category_name,category.score))
            points = [(round(l.x*width),round(l.y*height)) for l in landmarks]
            for a,b in HAND_EDGES:
                cv2.line(bgr, points[a], points[b], (80,180,240), 2)
            for point in points:
                cv2.circle(bgr, point, 3, (30,120,240), -1)
        top_gesture,top_score = choose_gesture(candidates)
        cv2.drawMarker(bgr, (width//2,height//2), (230,230,230), cv2.MARKER_CROSS, 25, 1)
        cv2.rectangle(bgr,(round(width*.44),round(height*.43)),
                      (round(width*.56),round(height*.57)),(130,130,130),1)
        text = f'Faces: {len(boxes)}  Gesture: {top_gesture or "-"} {top_score:.2f}'
        cv2.putText(bgr,text,(12,24),cv2.FONT_HERSHEY_SIMPLEX,.5,(240,240,240),1,cv2.LINE_AA)
        hand_points=tuple(tuple((float(p.x),float(p.y)*height/width,float(p.z)) for p in points)
                          for points in hands.hand_landmarks)
        return Observation(boxes, top_gesture, top_score, bgr, (time.perf_counter()-started)*1000,brightness,hand_points,
                           hashlib.blake2s(jpeg,digest_size=8).hexdigest(),
                           tuple(float(d.categories[0].score) for d in faces.detections))

    def close(self) -> None:
        self.gesture.close()
        self.face.close()
