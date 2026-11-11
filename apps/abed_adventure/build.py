#!/usr/bin/env python3
"""Abed's Adventure — original platformer.
    python apps/abed_adventure/build.py   -> build/apps/abed_adventure/abed_adventure.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("abed_adventure", [os.path.join(HERE, "main.c")], libc=True)
