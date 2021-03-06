// ChessCoach, a neural network-based chess engine capable of natural-language commentary
// Copyright 2021 Chris Butner
//
// ChessCoach is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ChessCoach is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ChessCoach. If not, see <https://www.gnu.org/licenses/>.

#ifndef _PGN_H_
#define _PGN_H_

#include <iostream>
#include <functional>
#include <string>

#include <Stockfish/position.h>

#include "SavedGame.h"

class Pgn
{
public:

    static std::tuple<int, int, int, int> ParsePgn(std::istream& content, bool allowNoResult, std::function<void(SavedGame&&, SavedCommentary&&)> gameHandler);
    static Move ParseSan(const Position& position, const std::string& san);

    static void GeneratePgn(std::ostream& content, const SavedGame& game);
    static std::string San(const std::string& fen, Move move, bool showCheckmate);
    static std::string San(const Position& position, Move move, bool showCheckmate);

private:

    static constexpr const char PieceSymbol[PIECE_TYPE_NB] = { '-', '-', 'N', 'B', 'R', 'Q', 'K', '-' };

private:

    static std::vector<float> GenerateMctsValues(const std::vector<uint16_t>& moves, float result);
    static std::vector<std::map<Move, float>> GenerateChildVisits(const std::vector<uint16_t>& moves);

    static bool ParseHeaders(std::istream& content, bool& fenGameInOut, float& resultOut);
    static void ParseHeader(std::istream& content, bool& fenGameInOut, float& resultOut);
    static bool ParseMoves(std::istream& content, StateListPtr& positionStates, Position& position, std::vector<uint16_t>& moves, SavedCommentary& commentary, float& resultInOut, bool inVariation);
    static void ParseComment(std::istream& content, const std::vector<uint16_t>& moves, SavedCommentary& commentary);
    static std::string ParseCommentInner(std::istream& content);
    static void ParseMoveGlyph(std::istream& content, std::string& target);
    static bool ParseVariation(std::istream& content, const Position& mainLine, const std::vector<uint16_t>& mainLineMoves, SavedCommentary& commentary, float& resultInOut);
    static bool Expect(std::istream& content, char expected);
    static bool Expect(std::istream& content, const std::string& expected);
    static void EncounterResult(float encountered, float& resultInOut);
    static void CheckResult(const Position& position, float& resultInOut, bool allowNoResult);
    static void SkipGame(std::istream& content);

    static bool ApplyMove(StateListPtr& positionStates, Position& position, Move move);
    static void UndoMoveInVariation(Position& position, Move move);
    static Move ParsePieceSan(const Position& position, const std::string& san, PieceType fromPieceType);
    static Move ParsePawnSan(const Position& position, const std::string& san);
    static Square ParseSquare(const std::string& text, int offset);
    static File ParseFile(const std::string& text, int offset);
    static Rank ParseRank(const std::string& text, int offset);
    static PieceType ParsePieceType(const std::string& text, int offset);

    static std::string SanPawn(const Position& position, Move move);
    static std::string SanPiece(const Position& position, Move move, Piece piece);

    static Bitboard Attacks(const Position& position, PieceType pieceType, Square targetSquare);
    static Bitboard Legal(const Position& position, Bitboard fromPieces, Square targetSquare);

    static inline char FileSymbol(File file) { return static_cast<char>('a' + file); }
    static inline char RankSymbol(Rank rank) { return static_cast<char>('1' + rank); }
};

#endif // _PGN_H_