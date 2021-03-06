#!/usr/bin/env bash
set -eux
pushd "$(dirname "$0")"

CHESSCOACH_DATA="${XDG_DATA_HOME-$HOME/.local/share}/ChessCoach"
CHESSCOACH_SYZYGY="${CHESSCOACH_DATA}/Syzygy"

cutechess-cli \
	-engine name=ChessCoach cmd=ChessCoachUci \
	-engine name=Stockfish_13 cmd=../tools/deb/stockfish_13_linux_x64_bmi2/stockfish_13_linux_x64_bmi2 \
		option.Threads=8 option.Hash=8192 option.SyzygyPath=${CHESSCOACH_SYZYGY} \
	-engine name=Stockfish_13_2850 cmd=../tools/deb/stockfish_13_linux_x64_bmi2/stockfish_13_linux_x64_bmi2 \
		option.Threads=1 \
		option.UCI_LimitStrength=true \
		option.UCI_Elo=2850 \
	-each proto=uci tc=300+3 timemargin=5000 \
	-games 4 \
	-pgnout "${CHESSCOACH_DATA}/tournament.pgn" \
	-recover

popd