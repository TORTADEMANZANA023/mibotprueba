#ifndef _SELFPLAY_H_
#define _SELFPLAY_H_

#include <map>
#include <vector>
#include <random>
#include <atomic>
#include <functional>

#include <Stockfish/Position.h>
#include <Stockfish/movegen.h>

#include "Game.h"
#include "Network.h"
#include "Storage.h"
#include "SavedGame.h"
#include "Threading.h"
#include "PredictionCache.h"
#include "PoolAllocator.h"
#include "Epd.h"

class Node
{
public:

    static const size_t BlockSizeBytes = 64 * 1024 * 1024; // 64 MiB
    thread_local static PoolAllocator<Node, BlockSizeBytes> Allocator;

public:

    Node(float setPrior);

    void* operator new(size_t byteCount);
    void operator delete(void* memory) noexcept;

    bool IsExpanded() const;
    float Value() const;

public:

    std::map<Move, Node*> children;
    std::pair<Move, Node*> mostVisitedChild;
    float originalPrior;
    float prior;
    int visitCount;
    int visitingCount;
    float valueSum;
    float terminalValue;
    bool expanding;
};

enum class SelfPlayState
{
    Working,
    WaitingForPrediction,
    Finished,
};

struct TimeControl
{
    bool infinite;
    int64_t moveTimeMs;

    int64_t timeRemainingMs[COLOR_NB];
    int64_t incrementMs[COLOR_NB];
};

struct SearchConfig
{
    std::mutex mutexUci;
    std::condition_variable signalUci;
    std::condition_variable signalReady;

    std::atomic_bool quit;
    std::atomic_bool debug;
    bool ready;

    std::atomic_bool searchUpdated;
    std::atomic_bool search;
    TimeControl searchTimeControl;

    std::atomic_bool positionUpdated;
    std::string positionFen;
    std::vector<Move> positionMoves;
};

struct SearchState
{
    std::string positionFen;
    std::vector<Move> positionMoves;
    bool searching;
    std::chrono::steady_clock::time_point searchStart;
    TimeControl timeControl;
    int nodeCount;
    int failedNodeCount;
    bool principleVariationChanged;
};

class SelfPlayGame : public Game
{
public:

    static std::atomic_uint ThreadSeed;
    thread_local static std::default_random_engine Random;

public:

    SelfPlayGame();
    SelfPlayGame(INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy);
    SelfPlayGame(const std::string& fen, const std::vector<Move>& moves, bool tryHard, INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy);

    SelfPlayGame(const SelfPlayGame& other);
    SelfPlayGame& operator=(const SelfPlayGame& other);
    SelfPlayGame(SelfPlayGame&& other) noexcept;
    SelfPlayGame& operator=(SelfPlayGame&& other) noexcept;
    ~SelfPlayGame();

    SelfPlayGame SpawnShadow(INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy);

    Node* Root() const;
    bool IsTerminal() const;
    float TerminalValue() const;
    float Result() const;
    
    bool TryHard() const;
    void ApplyMoveWithRoot(Move move, Node* newRoot);
    void ApplyMoveWithRootAndHistory(Move move, Node* newRoot);
    float ExpandAndEvaluate(SelfPlayState& state, PredictionCacheChunk*& cacheStore);
    void LimitBranchingToBest(int moveCount, uint16_t* moves, float* priors);
    bool IsDrawByNoProgressOrRepetition(int plyToSearchRoot);
    void Softmax(int moveCount, float* distribution) const;
    std::pair<Move, Node*> SelectMove() const;
    void StoreSearchStatistics();
    void Complete();
    SavedGame Save() const;
    void PruneExcept(Node* root, Node* except);
    void PruneAll();

    Move ParseSan(const std::string& san);

private:

    void PruneAllInternal(Node* root);

private:

    // Used for both real and scratch games.
    Node* _root;
    bool _tryHard;
    INetwork::InputPlanes* _image;
    float* _value;
    INetwork::OutputPlanes* _policy;
    int _searchRootPly;

    // Stored history and statistics.
    // Only used for real games, so no need to copy, but may make sense for primitives.
    std::vector<std::map<Move, float>> _childVisits;
    std::vector<Move> _history;
    float _result;

    // Coroutine state.
    // Only used for real games, so no need to copy.
    ExtMove _expandAndEvaluate_moves[MAX_MOVES];
    ExtMove* _expandAndEvaluate_endMoves;
    Key _imageKey;
    std::array<uint16_t, MAX_MOVES> _cachedMoves;
    std::array<float, MAX_MOVES> _cachedPriors;
};

class SelfPlayWorker
{
public:

    SelfPlayWorker();

    SelfPlayWorker(const SelfPlayWorker& other) = delete;
    SelfPlayWorker& operator=(const SelfPlayWorker& other) = delete;
    SelfPlayWorker(SelfPlayWorker&& other) = delete;
    SelfPlayWorker& operator=(SelfPlayWorker&& other) = delete;

    void ResetGames();
    void PlayGames(WorkCoordinator& workCoordinator, Storage* storage, INetwork* network);
    void Initialize(Storage* storage);
    void ClearGame(int index);
    void SetUpGame(int index);
    void SetUpGame(int index, const std::string& fen, const std::vector<Move>& moves, bool tryHard);
    void SetUpGameExisting(int index, const std::vector<Move>& moves, int applyNewMovesOffset, bool tryHard);
    void DebugGame(INetwork* network, int index, const SavedGame& saved, int startingPly);
    void TrainNetwork(INetwork* network, GameType gameType, int stepCount, int checkpoint);
    void TestNetwork(INetwork* network, int step);
    void Play(int index);
    void SaveToStorageAndLog(int index);
    std::pair<Move, Node*> RunMcts(SelfPlayGame& game, SelfPlayGame& scratchGame, SelfPlayState& state, int& mctsSimulation,
        std::vector<std::pair<Move, Node*>>& searchPath, PredictionCacheChunk*& cacheStore);
    void AddExplorationNoise(SelfPlayGame& game) const;
    std::pair<Move, Node*> SelectChild(const Node* node) const;
    float CalculateUcbScore(const Node* parent, const Node* child) const;
    void Backpropagate(const std::vector<std::pair<Move, Node*>>& searchPath, float value);

    void DebugGame(int index, SelfPlayGame** gameOut, SelfPlayState** stateOut, float** valuesOut, INetwork::OutputPlanes** policiesOut);
    SearchState& DebugSearchState();

    void Search(std::function<INetwork* ()> networkFactory);
    void WarmUpPredictions(INetwork* network, int batchSize);
    void SignalDebug(bool debug);
    void SignalPosition(std::string&& fen, std::vector<Move>&& moves);
    void SignalSearchGo(const TimeControl& timeControl);
    void SignalSearchStop();
    void SignalQuit();
    void WaitUntilReady();

    void StrengthTest(INetwork* network, int step);
    std::tuple<int, int, int> StrengthTest(INetwork* network, const std::filesystem::path& epdPath, int moveTimeMs);

private:

    void UpdatePosition();
    void UpdateSearch();
    void OnSearchFinished();
    void CheckPrintInfo();
    void CheckTimeControl();
    void PrintPrincipleVariation();
    void SearchPlay(int mctsParallelism);

    int StrengthTestPosition(INetwork* network, const StrengthTestSpec& spec, int moveTimeMs);
    int JudgeStrengthTestPosition(const StrengthTestSpec& spec, Move move);

private:

    Storage* _storage;

    std::vector<SelfPlayState> _states;
    std::vector<INetwork::InputPlanes> _images;
    std::vector<float> _values;
    std::vector<INetwork::OutputPlanes> _policies;

    std::vector<SelfPlayGame> _games;
    std::vector<SelfPlayGame> _scratchGames;
    std::vector<std::chrono::steady_clock::time_point> _gameStarts;
    std::vector<int> _mctsSimulations;
    std::vector<std::vector<std::pair<Move, Node*>>> _searchPaths;
    std::vector<PredictionCacheChunk*> _cacheStores;

    SearchConfig _searchConfig;
    SearchState _searchState;
};

#endif // _SELFPLAY_H_