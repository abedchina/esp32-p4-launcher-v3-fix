#!/usr/bin/env python3
"""Blink 33 - first student example: LED on GPIO 33 (launcher 0.33+).
    python apps/blink33/build.py   -> build/apps/blink33/blink33.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("blink33", [os.path.join(HERE, "main.c")], libc=True)
