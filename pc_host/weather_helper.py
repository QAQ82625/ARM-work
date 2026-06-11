#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Weather data helper — queries wttr.in free API (no key required)."""

import requests
import json

WEATHER_URL = "https://wttr.in/Shanghai?format=j1"
WEATHER_TIMEOUT = 10  # seconds

# Weather code mapping (wttr.in weatherDesc → MCU weather code)
# SUN=clear, CLD=cloudy, OVC=overcast, RAI=rain, SNO=snow, FOG=fog
WEATHER_CODES = {
    "sunny": "SUN", "clear": "SUN",
    "partly cloudy": "CLD", "cloudy": "CLD",
    "overcast": "OVC",
    "rain": "RAI", "light rain": "RAI", "moderate rain": "RAI",
    "heavy rain": "RAI", "drizzle": "RAI", "light drizzle": "RAI",
    "snow": "SNO", "light snow": "SNO", "moderate snow": "SNO",
    "fog": "FOG", "mist": "FOG",
}

def fetch_weather():
    """Query wttr.in and return (temperature_celsius, weather_code, description) or None."""
    try:
        resp = requests.get(WEATHER_URL, timeout=WEATHER_TIMEOUT)
        if resp.status_code != 200:
            return None
        data = resp.json()
        current = data.get("current_condition", [{}])[0]
        temp_c = int(current.get("temp_C", 0))
        desc = (current.get("weatherDesc", [{"value": "unknown"}])[0]["value"]).lower()
        wcode = "UNK"
        for keyword, code in WEATHER_CODES.items():
            if keyword in desc:
                wcode = code
                break
        return (temp_c, wcode, desc.capitalize())
    except Exception:
        return None

def fetch_weather_command():
    """Query weather and return *SET:WEA command string or None."""
    w = fetch_weather()
    if w is None:
        return None
    temp, wcode, desc = w
    return f"*SET:WEA {temp} {wcode}", desc
