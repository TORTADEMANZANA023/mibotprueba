# ChessCoach, a neural network-based chess engine capable of natural-language commentary
# Copyright 2021 Chris Butner
#
# ChessCoach is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# ChessCoach is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with ChessCoach. If not, see <https://www.gnu.org/licenses/>.

import http.server
import socketserver
import functools
import threading
import webbrowser
import websockets
import asyncio
import json
import logging
import os

import tensorflow as tf
import numpy as np

try:
  import chesscoach # See PythonModule.cpp
except:
  pass
import network
import suites
from model import ModelBuilder

# ----- WebSockets -----

serving = False
port = 8000
websocket_port = 8001
websocket_connected = set()
websocket_loop = None

class HttpHandler(http.server.SimpleHTTPRequestHandler):
  def log_message(self, format, *args):
    pass

def _serve():
  # This is a little hacky, but canonical InstallationScriptPath lives in C++, should work, be lazy for now.
  # Even hackier, prefer the dev location of files on Windows (e.g. from cpp\x64\Release to js).
  root = "../../../js" if os.path.exists("../../../js") else "js" if os.path.exists("js") else "../js"
  handler = functools.partial(HttpHandler, directory=root)
  with socketserver.TCPServer(("localhost", port), handler) as httpd:
    httpd.serve_forever()

def _websocket_serve():
  global websocket_loop
  websocket_loop = asyncio.new_event_loop()
  asyncio.set_event_loop(websocket_loop)
  start_server = websockets.serve(_websocket_handler, "localhost", websocket_port)
  websocket_loop.run_until_complete(start_server)
  websocket_loop.run_forever()

def _ensure_serving():
  global serving
  if not serving:
    serving = True
    threading.Thread(target=_serve).start()
    threading.Thread(target=_websocket_serve).start()
    websockets_logger = logging.getLogger("websockets.server")
    websockets_logger.setLevel(logging.CRITICAL)

async def _websocket_handler(websocket, path):
  try:
    websocket_connected.add(websocket)
    while True:
      message = await websocket.recv()
      await handle(json.loads(message))
  finally:
    websocket_connected.remove(websocket)

def dispatch(message):
  async def _send_one(websocket, message):
    try:
      return await websocket.send(message)
    except:
      pass

  def _send_all(websockets, message):
    if websockets:
      asyncio.create_task(asyncio.wait([_send_one(websocket, message) for websocket in websockets]))

  websocket_loop.call_soon_threadsafe(_send_all, websocket_connected, message)

async def send(message):
  await asyncio.wait([websocket.send(message) for websocket in websocket_connected])

# ----- Protocol -----

async def handle(message):
  if message["type"] == "hello":
    if gui_mode == "pull":
      await send(json.dumps({
        "type": "initialize",
        "game_count": game_count,
      }))
  elif message["type"] == "request":
    if gui_mode == "pull":
      requested_game = message.get("game")
      requested_position = message.get("position")
      await show_position(requested_game, requested_position)
  elif message["type"] == "commentary_request":
    if gui_mode == "pull":
      await comment_on_position()
  elif message["type"] == "commentary_suite_request":
    if gui_mode == "pull":
      await run_commentary_suite()
  elif message["type"] == "line":
    if gui_mode == "push":
      line = message.get("line")
      show_line(line)

# ----- Game and position data -----

config = network.config
games_per_chunk = config.misc["storage"]["games_per_chunk"]
chunks = tf.io.gfile.glob(network.trainer.data_glob_training)
game_count = len(chunks) * games_per_chunk
gui_mode = None

class Position:
  game = None
  chunk = None
  game_in_chunk = None
  position = None
  position_count = None
  pgn = None
  data = None

def clamp(value, min_value, max_value):
  return max(min_value, min(max_value, value))

async def show_position(requested_game, requested_position):
  # Handle game request.
  game = requested_game if requested_game is not None else Position.game if Position.game is not None else 0
  game = clamp(game, 0, game_count - 1)
  if game != Position.game and requested_position is None:
    requested_position = 0
  Position.game = game

  # Handle position request.
  position = requested_position if requested_position is not None else Position.position if Position.position is not None else 0 # Gets clamped in C++

  chunk = Position.game // games_per_chunk
  game_in_chunk = Position.game % games_per_chunk
  
  if chunk != Position.chunk:
    # Send chunk contents to C++.
    chunk_contents = tf.io.gfile.GFile(chunks[chunk], "rb").read()
    chesscoach.load_chunk(chunk_contents)
    Position.chunk = chunk
    Position.game_in_chunk = None
    Position.position = None

  if game_in_chunk != Position.game_in_chunk:
    # Parse game in C++.
    Position.position_count, Position.pgn = chesscoach.load_game(game_in_chunk)
    Position.game_in_chunk = game_in_chunk
    Position.position = None

  # C++ may update and clamp "position", e.g. if passing "-1" to represent the final position.
  if position != Position.position:
    # Get position data from C++.
    Position.position, *Position.data = chesscoach.load_position(position)
    
  # Send to JS.
  fen, evaluation, sans, froms, tos, targets = Position.data
  await send(json.dumps({
    "type": "training_data",
    "game": Position.game,
    "position_count": Position.position_count,
    "position": Position.position,
    "pgn": Position.pgn,
    "fen": fen,
    "evaluation": evaluation,
    "policy": [{ "san": san.decode("utf-8"), "from": move_from.decode("utf-8"), "to": move_to.decode("utf-8"), "target": round(float(target), 6)}
      for (san, move_from, move_to, target) in zip(sans, froms, tos, targets)],
  }))

def show_line(line):
  chesscoach.show_line(line.encode("utf-8"))

async def comment_on_position():
  # Generate input planes in C++ and predict commentary.
  image = chesscoach.generate_commentary_image_for_position(Position.position)
  variety_count = 5
  batch = tf.tile(image[None, :], [variety_count, 1])
  commentary = network.predict_commentary_batch(batch)

  # Send to JS.
  await send(json.dumps({
    "type": "commentary_response",
    "commentary": [comment.decode("utf-8") for comment in commentary],
  }))

async def run_commentary_suite():
  # Grab suite positions and baselines, generate input planes in C++ and predict commentary.
  suite = suites.commentary
  images = np.array([chesscoach.generate_commentary_image_for_fens(item["before"].encode("utf-8"), item["after"].encode("utf-8")) for item in suite])
  suite_count = len(suite)
  variety_count = 5
  batch = tf.tile(images[None, :, :], [variety_count, 1, 1])
  batch = tf.transpose(batch, [1, 0, 2])
  batch = tf.reshape(batch, [suite_count * variety_count, ModelBuilder.commentary_input_planes_count])
  commentary = network.predict_commentary_batch(batch)
  commentary = np.reshape(commentary, [suite_count, variety_count]).tolist()

  # Send to JS.
  await send(json.dumps({
    "type": "commentary_suite_response",
    "items": suite,
    "commentary": [[comment.decode("utf-8") for comment in comments] for comments in commentary],
  }))

# ----- API -----

def launch(mode):
  global gui_mode
  gui_mode = mode
  _ensure_serving()
  webbrowser.open(f"http://localhost:{port}/gui.html")

def update(fen, line, node_count, evaluation, principal_variation, sans, froms, tos, targets, priors, values, puct, visits, weights):
  assert gui_mode == "push"
  dispatch(json.dumps({
    "type": "uci_data",
    "fen": fen,
    "line": line,
    "node_count": node_count,
    "evaluation": evaluation,
    "principal_variation": principal_variation,
    "policy": [{ "san": san.decode("utf-8"), "from": move_from.decode("utf-8"), "to": move_to.decode("utf-8"), "target": round(float(target), 6),
      "prior": round(float(prior), 6), "value": round(float(value), 6), "puct": round(float(puct), 6), "visits": int(visits), "weight": int(weight)}
      for (san, move_from, move_to, target, prior, value, puct, visits, weight) in zip(sans, froms, tos, targets, priors, values, puct, visits, weights)],
  }))

# -----

# Example training data format (pull mode):
#
# {
#   "type": "training_data",
#   "game": 2,
#   "position_count": 8,
#   "position": 4,
#   "pgn": "...",
#   "fen": "3rkb1r/p2nqppp/5n2/1B2p1B1/4P3/1Q6/PPP2PPP/2KR3R w k - 3 13",
#   "evaluation": "0.5 (0.0 pawns)",
#   "policy": [
#     { "san": "e4", "from": "e2", "to": "e4", "target": 0.25 },
#     { "san": "d4", "from": "d2", "to": "d4", "target": 0.75 },
#   ]
# }
#
# Example UCI data format (push mode):
#
# {
#   "type": "uci_data",
#   "fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
#   "line": "",
#   "node_count": 350024,
#   "evaluation": "0.527982 (0.0979088 pawns)",
#   "principal_variation": "d4 d5 c4 c6 Nf3 Nf6 Nc3 e6 e3 Nbd7 Qc2 Bd6 Bd3 O-O O-O dxc4 Bxc4 b5 Be2 Bb7 e4 e5 Rd1 Qc7 dxe5 Nxe5 Nxe5 Bxe5 g3 ",
#   "policy": [
#     {"san": "a3", "from": "a2", "to": "a3", "target": 0.000583, ...},
#     {"san": "b3", "from": "b2", "to": "b3", "target": 0.001246, ...},
#     ...
#   ]
# }