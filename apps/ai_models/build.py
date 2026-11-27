#!/usr/bin/env python3
"""AI Models - pick an ESP-DL model from /sd/models and run it on the USB camera (launcher 0.31+).
    python apps/ai_models/build.py   -> build/apps/ai_models/ai_models.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("ai_models", [os.path.join(HERE, "main.c")], libc=True)
