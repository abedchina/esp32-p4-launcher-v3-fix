#!/usr/bin/env python3
"""Hand Robot - shows the hand-gesture robot (detection + live video) and drives it with the joystick.
    python apps/hand_robot/build.py   -> build/apps/hand_robot/hand_robot.papp
Needs apps/hand_robot/secret.h (copy secret.example.h)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("hand_robot",
                 [os.path.join(HERE, "main.c"), os.path.join(HERE, "tjpgd.c")],
                 includes=[HERE], libc=True)
