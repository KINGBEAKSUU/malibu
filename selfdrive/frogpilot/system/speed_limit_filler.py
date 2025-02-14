#!/usr/bin/env python3
import json
import math
import requests
import time

from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed

import openpilot.system.sentry as sentry

from cereal import log, messaging

from openpilot.selfdrive.frogpilot.frogpilot_utilities import calculate_distance_to_point, is_url_pingable
from openpilot.selfdrive.frogpilot.frogpilot_variables import params, params_memory

GPS_SEARCH_RANGE = 0.0001
MAX_ENTRIES = 10_000_000

KUMI_API_URL = "https://overpass.kumi.systems/api/interpreter"
OVERPASS_API_URL = "https://overpass-api.de/api/interpreter"

def add_entry(dataset, entry):
  dataset.append(entry)

def cleanup_dataset(dataset):
  cleaned_dataset = deque(maxlen=MAX_ENTRIES)
  unique_entries = set()

  for entry in dataset:
    entry_str = json.dumps(entry, sort_keys=True)
    if entry_str not in unique_entries:
      unique_entries.add(entry_str)
      cleaned_dataset.append(entry)

  return cleaned_dataset

class MapSpeedLogger:
  def __init__(self):
    self.speed_limits_checked = False
    self.started_previously = False

    self.previous_coords = None

    self.dataset = deque(json.loads(params.get("SpeedLimits") or "[]"), maxlen=MAX_ENTRIES)
    self.filtered_dataset = deque(json.loads(params.get("SpeedLimitsFiltered") or "[]"), maxlen=MAX_ENTRIES)

    self.sm = messaging.SubMaster(["deviceState", "frogpilotCarState", "frogpilotNavigation", "liveLocationKalman"])

  def log_speed_limit(self):
    self.sm.update()

    if not self.sm["deviceState"].started and self.started_previously:
      params.put("SpeedLimits", json.dumps(list(self.dataset)))
      self.speed_limits_checked = False
      self.previous_coords = None

    self.started_previously = self.sm["deviceState"].started

    if not self.sm.updated["liveLocationKalman"]:
      return

    localizer_valid = (self.sm["liveLocationKalman"].status == log.LiveLocationKalman.Status.valid) and self.sm["liveLocationKalman"].positionGeodetic.valid
    if not (self.sm["liveLocationKalman"].gpsOK and localizer_valid):
      self.previous_coords = None
      return

    if params_memory.get_float("MapSpeedLimit") != 0:
      self.previous_coords = None
      return

    current_latitude = self.sm["liveLocationKalman"].positionGeodetic.value[0]
    current_longitude = self.sm["liveLocationKalman"].positionGeodetic.value[1]

    if self.previous_coords is not None:
      start_latitude, start_longitude = map(math.radians, [self.previous_coords["latitude"], self.previous_coords["longitude"]])
      end_latitude, end_longitude = map(math.radians, [current_latitude, current_longitude])
      distance = calculate_distance_to_point(start_latitude, start_longitude, end_latitude, end_longitude)

      if distance < 1:
        return
    else:
      self.previous_coords = {"latitude": current_latitude, "longitude": current_longitude}
      return

    dashboard_speed = self.sm["frogpilotCarState"].dashboardSpeedLimit
    navigation_speed = self.sm["frogpilotNavigation"].navigationSpeedLimit

    if dashboard_speed:
      add_entry(self.dataset, {
        "start_coordinates": self.previous_coords,
        "end_coordinates": {"latitude": current_latitude, "longitude": current_longitude},
        "speed_limit": dashboard_speed,
        "source": "Dashboard"
      })

    elif navigation_speed:
      add_entry(self.dataset, {
        "start_coordinates": self.previous_coords,
        "end_coordinates": {"latitude": current_latitude, "longitude": current_longitude},
        "speed_limit": navigation_speed,
        "source": "NOO"
      })

    self.previous_coords = {"latitude": current_latitude, "longitude": current_longitude}

  def fetch_segments_from_overpass(self, start_coords, end_coords):
    road_types = "(motorway|motorway_link|primary|primary_link|residential|secondary|secondary_link|tertiary|tertiary_link|trunk|trunk_link)"

    min_lat = min(start_coords["latitude"], end_coords["latitude"]) - GPS_SEARCH_RANGE
    max_lat = max(start_coords["latitude"], end_coords["latitude"]) + GPS_SEARCH_RANGE
    min_lon = min(start_coords["longitude"], end_coords["longitude"]) - GPS_SEARCH_RANGE
    max_lon = max(start_coords["longitude"], end_coords["longitude"]) + GPS_SEARCH_RANGE

    for attempt in range(10):
      query = (
        f"[out:json]; "
        f"way({min_lat},{min_lon},{max_lat},{max_lon})[highway~'{road_types}']; "
        f"out body; >; out skel qt;"
      )

      for api_url in [OVERPASS_API_URL, KUMI_API_URL]:
        try:
          response = requests.get(api_url, params={"data": query}, timeout=10)
          response.raise_for_status()

          data = response.json()
          ways = [element for element in data.get("elements", []) if element.get("type") == "way"]
          return [(segment.get("id"), segment.get("tags", {}).get("maxspeed")) for segment in ways]
        except Exception as e:
          print(f"Error fetching from {api_url}: {e}")

      min_lat -= GPS_SEARCH_RANGE
      max_lat += GPS_SEARCH_RANGE
      min_lon -= GPS_SEARCH_RANGE
      max_lon += GPS_SEARCH_RANGE

    return None

  def fetch_speed_limit_for_segment_id(self, segment_id):
    query = f"[out:json]; way({segment_id}); out tags;"

    for api_url in [OVERPASS_API_URL, KUMI_API_URL]:
      try:
        response = requests.get(api_url, params={"data": query}, timeout=10)
        response.raise_for_status()

        data = response.json()
        ways = [element for element in data.get("elements", []) if element.get("type") == "way"]
        return ways[0].get("tags", {}).get("maxspeed") if ways else None
      except Exception as e:
        print(f"Error fetching speed limit from {api_url}: {e}")

    return None

  def update_speed_limits(self):
    if not self.dataset:
      return

    self.dataset = cleanup_dataset(self.dataset)
    self.filtered_dataset = cleanup_dataset(self.filtered_dataset)

    while not is_url_pingable("https://overpass-api.de"):
      self.sm.update()

      if self.sm["deviceState"].started:
        return

      time.sleep(1)

    filtered_vetted = deque(maxlen=MAX_ENTRIES)
    for entry in self.filtered_dataset:
      self.sm.update()

      if self.sm["deviceState"].started:
        return

      segment_id = entry.get("segment_id")
      if segment_id and self.fetch_speed_limit_for_segment_id(segment_id) is None:
        filtered_vetted.append(entry)

    self.filtered_dataset = filtered_vetted

    total_entries = len(self.dataset)
    existing_segment_ids = {entry["segment_id"] for entry in self.filtered_dataset if "segment_id" in entry}

    def process_entry(entry):
      start_coords = entry.get("start_coordinates")
      end_coords = entry.get("end_coordinates")
      if not start_coords or not end_coords:
        return None, None

      result = self.fetch_segments_from_overpass(start_coords, end_coords)
      return entry, result

    with ThreadPoolExecutor(max_workers=5) as executor:
      futures = {executor.submit(process_entry, entry): entry for entry in list(self.dataset)}

      for count, future in enumerate(as_completed(futures), start=1):
        print(f"Processing entry {count}/{total_entries}")

        self.sm.update()

        if self.sm["deviceState"].started:
          break

        entry, result = future.result()
        if not entry:
          continue

        if result is not None:
          self.dataset.remove(entry)

          for segment_id, speed_limit in result:
            if not segment_id or segment_id in existing_segment_ids or speed_limit:
              continue

            add_entry(self.filtered_dataset, {
              "segment_id": segment_id,
              "source": entry.get("source"),
              "speed_limit": entry.get("speed_limit"),
            })

            existing_segment_ids.add(segment_id)

        if count % 100 == 0:
          params.put("SpeedLimits", json.dumps(list(self.dataset)))
          params.put("SpeedLimitsFiltered", json.dumps(list(deque(sorted(self.filtered_dataset, key=lambda entry: entry["segment_id"]), maxlen=MAX_ENTRIES))))

    params.put("SpeedLimits", json.dumps(list(self.dataset)))
    params.put("SpeedLimitsFiltered", json.dumps(list(deque(sorted(self.filtered_dataset, key=lambda entry: entry["segment_id"]), maxlen=MAX_ENTRIES))))

    self.speed_limits_checked = True

def main():
  logger = MapSpeedLogger()

  while True:
    try:
      if not logger.speed_limits_checked:
        logger.update_speed_limits()

      logger.log_speed_limit()
    except Exception as error:
      print(f"Error in speed_limit_filler: {error}")
      sentry.capture_exception(error)

if __name__ == "__main__":
  main()
