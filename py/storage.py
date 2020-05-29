import os
import struct
import numpy
import time

def load_latest_network(config, name, network_path_handler):
  parent_path = config.misc["paths"]["networks"]
  _, directories, _ = next(os.walk(parent_path))
  for directory in reversed(directories): # Only load the latest.
    if directory.startswith(name + "_"):
      return network_path_handler(os.path.join(parent_path, directory))
  return None

def save_network(config, name, step, network):
  parent_path = config.misc["paths"]["networks"]
  directory_name = f"{name}_{str(step).zfill(9)}"
  path = os.path.join(parent_path, directory_name)
  # Don't serialize optimizer: custom loss/metrics.
  network.model.save(path, include_optimizer=False, save_format="tf")
  return path