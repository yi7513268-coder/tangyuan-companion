"""Infer axis direction from static-scene image registration, without face identity."""

import cv2
import numpy as np


def image_shift(before: bytes, after: bytes) -> tuple[float, float, int]:
    """Robust scene displacement, requiring sufficient feature matches/inliers."""
    frames = [cv2.imdecode(np.frombuffer(b,np.uint8),cv2.IMREAD_GRAYSCALE) for b in (before,after)]
    if any(f is None for f in frames):
        raise ValueError('Invalid calibration JPEG')
    clahe = cv2.createCLAHE(clipLimit=3,tileGridSize=(8,8))
    detector = cv2.ORB_create(nfeatures=1200,fastThreshold=5)
    keypoints,descriptors = zip(*(detector.detectAndCompute(clahe.apply(f),None) for f in frames))
    if any(d is None for d in descriptors):
        raise ValueError('Insufficient scene texture for axis calibration')
    matches = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(descriptors[0],descriptors[1],k=2)
    good = [a for pair in matches if len(pair)==2 for a,b in [pair] if a.distance < .7*b.distance]
    if len(good)<12:
        raise ValueError('Insufficient reliable matches; use a well-lit static scene')
    source = np.float32([keypoints[0][m.queryIdx].pt for m in good])
    target = np.float32([keypoints[1][m.trainIdx].pt for m in good])
    _,inliers = cv2.estimateAffinePartial2D(source,target,method=cv2.RANSAC,ransacReprojThreshold=3)
    if inliers is None or int(inliers.sum())<10:
        raise ValueError('Scene was moving or calibration matches were inconsistent')
    displacement = np.median((target-source)[inliers.ravel().astype(bool)],axis=0)
    return float(displacement[0]),float(displacement[1]),int(inliers.sum())


def axis_sign(displacement: float) -> int:
    if abs(displacement)<6:
        raise ValueError('Axis movement too small to determine direction')
    # The motor should turn opposite to the resulting scene displacement.
    return -1 if displacement>0 else 1
