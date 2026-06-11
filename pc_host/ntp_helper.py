#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NTP time sync helper — queries ntp.aliyun.com, returns UTC+8 Beijing time."""

import ntplib
import time
from datetime import datetime, timezone, timedelta

NTP_SERVER = "ntp.aliyun.com"
NTP_TIMEOUT = 3  # seconds

def fetch_ntp_time():
    """Query NTP server and return (year, month, day, hour, minute, second) or None."""
    client = ntplib.NTPClient()
    try:
        response = client.request(NTP_SERVER, timeout=NTP_TIMEOUT)
        ts = response.tx_time
        dt = datetime.fromtimestamp(ts, tz=timezone.utc) + timedelta(hours=8)
        return (dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)
    except Exception:
        return None

def fetch_ntp_commands():
    """Query NTP and return the *SET: commands to sync MCU time. Returns list of cmd strings or None."""
    t = fetch_ntp_time()
    if t is None:
        return None
    y, mo, d, h, mi, s = t
    return [
        f"*SET:DATE YEAR {y} MONTH {mo} DATE {d}",
        f"*SET:TIME HOUR {h} MIN {mi} SEC {s}",
    ]

def format_time(year, month, day, hour, minute, second):
    return f"{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}"
