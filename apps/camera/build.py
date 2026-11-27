#!/usr/bin/env python3
"""Camera - live USB camera view (launcher 0.30+).
    python apps/camera/build.py   -> build/apps/camera/camera.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("camera", [os.path.join(HERE, "main.c")], libc=True)
