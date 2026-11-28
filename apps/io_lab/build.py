#!/usr/bin/env python3
"""IO Lab - the pin header on screen: set, read and test every GPIO (launcher 0.33+).
    python apps/io_lab/build.py   -> build/apps/io_lab/io_lab.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("io_lab", [os.path.join(HERE, "main.c")], libc=True)
