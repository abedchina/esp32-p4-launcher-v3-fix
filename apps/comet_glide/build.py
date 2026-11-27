#!/usr/bin/env python3
"""Comet Glide - original arcade game by Abed Mihilia.
    python apps/comet_glide/build.py   -> build/apps/comet_glide/comet_glide.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("comet_glide", [os.path.join(HERE, "main.c")], libc=True)
