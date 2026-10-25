"""Behavioral tests for the host vision controller, without robot hardware."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from control import Face, FaceFollower, GestureGate


def test_centered_face_has_no_servo_correction():
    follower = FaceFollower()
    assert follower.update([Face(.4, .4, .2, .2)], now=0) is None
    assert follower.update([Face(.4, .4, .2, .2)], now=.2) is None


def test_correction_is_rate_limited_and_stays_inside_joint_limits():
    follower = FaceFollower(pan_sign=1, tilt_sign=1)
    for i in range(200):
        target = follower.update([Face(.8, .8, .1, .1)], now=i*.2)
        if target:
            assert 30 <= target[0] <= 150
            assert 100 <= target[1] <= 130
    assert follower.pan == 150
    assert follower.tilt == 130


def test_axis_direction_can_be_reversed():
    a, b = FaceFollower(pan_sign=1), FaceFollower(pan_sign=-1)
    for t in (0, .2, .4):
        a.update([Face(.8, .4, .1, .2)], now=t)
        b.update([Face(.8, .4, .1, .2)], now=t)
    assert a.pan > 90 > b.pan


def test_lost_target_never_drives_or_integrates_elapsed_time():
    follower = FaceFollower(pan_sign=1)
    follower.update([Face(.8, .4, .1, .2)], now=0)
    follower.update([Face(.8, .4, .1, .2)], now=.2)
    before = follower.pan
    assert follower.update([], now=20) is None
    assert follower.update([Face(.8, .4, .1, .2)], now=21) is None
    assert follower.pan == before


def test_keeps_nearby_target_when_a_larger_second_face_arrives():
    follower = FaceFollower()
    first = Face(.15, .4, .2, .2)
    follower.update([first], now=0)
    follower.update([first, Face(.65, .2, .3, .4)], now=.2)
    assert follower.target == first


def test_new_target_is_not_acquired_until_missing_target_timeout():
    follower = FaceFollower()
    follower.update([Face(.1, .4, .2, .2)], now=0)
    assert follower.update([Face(.7, .4, .2, .2)], now=.2) is None
    follower.update([Face(.7, .4, .2, .2)], now=1.5)
    assert follower.target.x == .7


def test_gesture_requires_confidence_stability_and_cooldown():
    gate = GestureGate(hold_seconds=.4, cooldown=2)
    assert gate.update('Thumb_Up', .95, now=0) is None
    assert gate.update('Thumb_Up', .95, now=.2) is None
    assert gate.update('Thumb_Up', .95, now=.5) == 'Thumb_Up'
    assert gate.update('Thumb_Up', .95, now=4) is None
    assert gate.update(None, 0, now=4.1) is None
    assert gate.update(None, 0, now=4.7) is None
    assert gate.update('Thumb_Up', .95, now=5) is None
    assert gate.update('Thumb_Up', .95, now=5.5) == 'Thumb_Up'


def test_unknown_or_low_confidence_gesture_does_not_trigger():
    gate = GestureGate()
    for t in (0, 1, 2):
        assert gate.update('Thumb_Up', .2, now=t) is None
        assert gate.update('None', .99, now=t) is None


def test_no_stability_credit_from_a_gap_between_frames():
    gate = GestureGate()
    gate.update('Victory', .99, now=0)
    assert gate.update('Victory', .99, now=8) is None


def test_fist_pause_is_not_blocked_by_a_recent_response():
    gate = GestureGate()
    gate.update('Thumb_Up', .99, now=0)
    assert gate.update('Thumb_Up', .99, now=.6) == 'Thumb_Up'
    gate.update('Closed_Fist', .99, now=.7)
    assert gate.update('Closed_Fist', .99, now=1.3) == 'Closed_Fist'


def test_no_face_correction_from_a_frame_after_a_long_gap():
    follower = FaceFollower(pan_sign=1)
    follower.update([Face(.8,.4,.1,.2)],now=0)
    follower.update([Face(.8,.4,.1,.2)],now=.2)
    before = follower.pan
    assert follower.update([Face(.8,.4,.1,.2)],now=10) is None
    assert follower.pan == before


def test_nonfinite_confidence_never_triggers_gesture():
    gate = GestureGate()
    gate.update('Thumb_Up', float('nan'), now=0)
    assert gate.update('Thumb_Up', float('nan'), now=1) is None


@pytest.mark.parametrize('pan,tilt', [(0,115), (90,180)])
def test_reset_clamps_servo_reference(pan, tilt):
    follower = FaceFollower()
    follower.reset(pan, tilt)
    assert 30 <= follower.pan <= 150
    assert 100 <= follower.tilt <= 130
