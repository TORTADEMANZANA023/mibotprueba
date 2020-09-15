#include <gtest/gtest.h>

#include <functional>

#include <ChessCoach/SelfPlay.h>
#include <ChessCoach/ChessCoach.h>

SelfPlayGame& PlayGame(SelfPlayWorker& selfPlayWorker, std::function<void (SelfPlayGame&)> tickCallback)
{
    const int index = 0;
    SelfPlayGame* game;
    SelfPlayState* state;
    float* values;
    INetwork::OutputPlanes* policies;

    selfPlayWorker.DebugGame(index, &game, &state, &values, &policies);

    selfPlayWorker.SetUpGame(index);

    while (true)
    {
        // CPU work
        selfPlayWorker.Play(index);

        if (*state == SelfPlayState::Finished)
        {
            return *game;
        }

        // "GPU" work. Pretend to predict for a batch.
        std::fill(values, values + selfPlayWorker.Config().SelfPlay.PredictionBatchSize, CHESSCOACH_VALUE_DRAW);

        float* policiesPtr = reinterpret_cast<float*>(policies);
        const int policyCount = (selfPlayWorker.Config().SelfPlay.PredictionBatchSize * INetwork::OutputPlanesFloatCount);
        std::fill(policiesPtr, policiesPtr + policyCount, 0.f);

        tickCallback(*game);
    }
}

std::vector<Node*> GeneratePrincipleVariation(const SelfPlayWorker& selfPlayWorker, const SelfPlayGame& game)
{
    Node* node = game.Root();
    std::vector<Node*> principleVariation;

    while (node)
    {
        for (const Node& child : *node)
        {
            if (child.visitCount > 0)
            {
                const bool bestIsNotBest = selfPlayWorker.WorseThan(node->bestChild, &child);
                if (bestIsNotBest) throw std::runtime_error("bestIsNotBest false");
            }
        }
        if (node->bestChild)
        {
            principleVariation.push_back(node->bestChild);
        }
        node = node->bestChild;
    }

    return principleVariation;
}

void MockExpand(Node* node, int count)
{
    const float prior = (1.f / count);

    Node* lastSibling = nullptr;
    for (int i = 0; i < count; i++)
    {
        Node* child = new Node(Move(i), prior);
        if (lastSibling)
        {
            lastSibling->nextSibling = child;
        }
        else
        {
            node->firstChild = child;
        }
        lastSibling = child;
    }
}

void CheckMateN(Node* node, int n)
{
    assert(n >= 1);

    EXPECT_EQ(node->terminalValue.IsImmediate(), (n == 1));
    EXPECT_EQ(node->terminalValue.ImmediateValue(), (n == 1) ? CHESSCOACH_VALUE_WIN : CHESSCOACH_VALUE_DRAW);
    EXPECT_EQ(node->terminalValue.IsMateInN(), true);
    EXPECT_EQ(node->terminalValue.IsOpponentMateInN(), false);
    EXPECT_EQ(node->terminalValue.MateN(), n);
    EXPECT_EQ(node->terminalValue.OpponentMateN(), 0);
    EXPECT_EQ(node->terminalValue.EitherMateN(), n);
}

void CheckOpponentMateN(Node* node, int n)
{
    assert(n >= 1);

    EXPECT_EQ(node->terminalValue.IsImmediate(), false);
    EXPECT_EQ(node->terminalValue.ImmediateValue(), CHESSCOACH_VALUE_DRAW);
    EXPECT_EQ(node->terminalValue.IsMateInN(), false);
    EXPECT_EQ(node->terminalValue.IsOpponentMateInN(), true);
    EXPECT_EQ(node->terminalValue.MateN(), 0);
    EXPECT_EQ(node->terminalValue.OpponentMateN(), n);
    EXPECT_EQ(node->terminalValue.EitherMateN(), -n);
}

void CheckDraw(Node* node)
{
    EXPECT_EQ(node->terminalValue.IsImmediate(), true);
    EXPECT_EQ(node->terminalValue.ImmediateValue(), CHESSCOACH_VALUE_DRAW);
    EXPECT_EQ(node->terminalValue.IsMateInN(), false);
    EXPECT_EQ(node->terminalValue.IsOpponentMateInN(), false);
    EXPECT_EQ(node->terminalValue.MateN(), 0);
    EXPECT_EQ(node->terminalValue.OpponentMateN(), 0);
    EXPECT_EQ(node->terminalValue.EitherMateN(), 0);
}

void CheckNonTerminal(Node* node)
{
    EXPECT_EQ(node->terminalValue.IsImmediate(), false);
    EXPECT_EQ(node->terminalValue.ImmediateValue(), CHESSCOACH_VALUE_DRAW);
    EXPECT_EQ(node->terminalValue.IsMateInN(), false);
    EXPECT_EQ(node->terminalValue.IsOpponentMateInN(), false);
    EXPECT_EQ(node->terminalValue.MateN(), 0);
    EXPECT_EQ(node->terminalValue.OpponentMateN(), 0);
    EXPECT_EQ(node->terminalValue.EitherMateN(), 0);
}

TEST(Mcts, NodeLeaks)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayWorker selfPlayWorker(Config::UciNetwork, nullptr /* storage */);

// Allocations are only tracked with DEBUG.
#ifdef DEBUG
    auto [currentBefore, peakBefore] = Node::Allocator.DebugAllocations();
    EXPECT_EQ(currentBefore, 0);
    EXPECT_EQ(peakBefore, 0);
#endif

    PlayGame(selfPlayWorker, [](auto&) {});

// Allocations are only tracked with DEBUG.
#ifdef DEBUG
    auto [currentAfter, peakAfter] = Node::Allocator.DebugAllocations();
    EXPECT_EQ(currentAfter, 0);
    EXPECT_GT(peakAfter, 0);
#endif
}

TEST(Mcts, PrincipleVariation)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayWorker selfPlayWorker(Config::UciNetwork, nullptr /* storage */);
    SearchState& searchState = selfPlayWorker.DebugSearchState();

    std::vector<Node*> latestPrincipleVariation;
    PlayGame(selfPlayWorker, [&](SelfPlayGame& game)
        {
            std::vector<Node*> principleVariation = GeneratePrincipleVariation(selfPlayWorker, game);
            if (searchState.principleVariationChanged)
            {
                EXPECT_NE(principleVariation, latestPrincipleVariation);
                searchState.principleVariationChanged = false;
            }
            else
            {
                EXPECT_EQ(principleVariation, latestPrincipleVariation);
            }
            latestPrincipleVariation = principleVariation;
        });
}

TEST(Mcts, MateComparisons)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayWorker selfPlayWorker(Config::UciNetwork, nullptr /* storage */);
    SelfPlayGame* game;
    selfPlayWorker.SetUpGame(0);
    selfPlayWorker.DebugGame(0, &game, nullptr, nullptr, nullptr);

    // Set up nodes from expected worst to best.
    const int nodeCount = 7;
    Node nodes[nodeCount] = { {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f}, {MOVE_NONE, 0.f} };
    nodes[0].terminalValue = TerminalValue::OpponentMateIn<2>();
    nodes[1].terminalValue = TerminalValue::OpponentMateIn<4>();
    nodes[2].visitCount = 10;
    nodes[3].terminalValue = TerminalValue::Draw();
    nodes[3].visitCount = 15;
    nodes[4].visitCount = 100;
    nodes[5].terminalValue = TerminalValue::MateIn<3>();
    nodes[6].terminalValue = TerminalValue::MateIn<1>();

    // Check all pairs.
    for (int i = 0; i < nodeCount - 1; i++)
    {
        EXPECT_FALSE(selfPlayWorker.WorseThan(&nodes[i], &nodes[i]));

        EXPECT_TRUE(selfPlayWorker.WorseThan(nullptr, &nodes[i]));

        for (int j = i + 1; j < nodeCount; j++)
        {
            EXPECT_TRUE(selfPlayWorker.WorseThan(&nodes[i], &nodes[j]));
            EXPECT_FALSE(selfPlayWorker.WorseThan(&nodes[j], &nodes[i]));
        }
    }
}

TEST(Mcts, MateProving)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayWorker selfPlayWorker(Config::UciNetwork, nullptr /* storage */);
    SelfPlayGame* game;
    selfPlayWorker.SetUpGame(0);
    selfPlayWorker.DebugGame(0, &game, nullptr, nullptr, nullptr);

    // Expand a small tree (1 root, 3 ply1, 9 ply2).
    MockExpand(game->Root(), 3);
    MockExpand(game->Root()->Child(Move(0)), 3);
    MockExpand(game->Root()->Child(Move(1)), 3);
    MockExpand(game->Root()->Child(Move(2)), 3);

    // Selectively deepen two leaves.
    MockExpand(game->Root()->Child(Move(1))->Child(Move(1)), 1);
    MockExpand(game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0)), 1);
    MockExpand(game->Root()->Child(Move(2))->Child(Move(2)), 1);
    MockExpand(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0)), 1);
    MockExpand(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0)), 1);
    MockExpand(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0)), 1);

    // Expect that root and ply2child0 are non-terminal.
    CheckNonTerminal(game->Root());
    CheckNonTerminal(game->Root()->Child(Move(0))->Child(Move(0)));

    // Make ply2child0 a mate-in-1 (M1) and backpropagate.
    game->Root()->Child(Move(0))->Child(Move(0))->terminalValue = TerminalValue::MateIn<1>();
    selfPlayWorker.BackpropagateMate({
        { game->Root()},
        { game->Root()->Child(Move(0)) },
        { game->Root()->Child(Move(0))->Child(Move(0)) }});
    CheckMateN(game->Root()->Child(Move(0))->Child(Move(0)), 1);
    CheckOpponentMateN(game->Root()->Child(Move(0)), 1);
    CheckNonTerminal(game->Root());

    // Make ply2child1 a draw.
    game->Root()->Child(Move(0))->Child(Move(1))->terminalValue = TerminalValue::Draw();
    CheckDraw(game->Root()->Child(Move(0))->Child(Move(1)));

    // Make ply2child5 a mate-in-2 (M2) and backpropagate.
    game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0))->Child(Move(0))->terminalValue = TerminalValue::MateIn<1>();
    selfPlayWorker.BackpropagateMate({
        { game->Root()},
        { game->Root()->Child(Move(1)) },
        { game->Root()->Child(Move(1))->Child(Move(1)) },
        { game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0)) },
        { game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0))->Child(Move(0)) }});
    CheckMateN(game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0))->Child(Move(0)), 1);
    CheckOpponentMateN(game->Root()->Child(Move(1))->Child(Move(1))->Child(Move(0)), 1);
    CheckMateN(game->Root()->Child(Move(1))->Child(Move(1)), 2);
    CheckOpponentMateN(game->Root()->Child(Move(1)), 2);
    CheckNonTerminal(game->Root());

    // Make ply2child8 a mate-in-3 (M3) and backpropagate.
    // This should cause the root to get recognized as a mate-in-4 (M4).
    game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0))->Child(Move(0))->terminalValue = TerminalValue::MateIn<1>();
    selfPlayWorker.BackpropagateMate({
        { game->Root()},
        { game->Root()->Child(Move(2)) },
        { game->Root()->Child(Move(2))->Child(Move(2)) },
        { game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0)) },
        { game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0)) },
        { game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0)) },
        { game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0))->Child(Move(0)) }});
    CheckMateN(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0))->Child(Move(0)), 1);
    CheckOpponentMateN(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0))->Child(Move(0)), 1);
    CheckMateN(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0))->Child(Move(0)), 2);
    CheckOpponentMateN(game->Root()->Child(Move(2))->Child(Move(2))->Child(Move(0)), 2);
    CheckMateN(game->Root()->Child(Move(2))->Child(Move(2)), 3);
    CheckOpponentMateN(game->Root()->Child(Move(2)), 3);
    CheckMateN(game->Root(), 4);

    game->PruneAll();
}

TEST(Mcts, TwofoldRepetition)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayWorker selfPlayWorker(Config::UciNetwork, nullptr /* storage */);
    SelfPlayGame* game;
    selfPlayWorker.SetUpGame(0);
    selfPlayWorker.DebugGame(0, &game, nullptr, nullptr, nullptr);

    // Set up a simple 2-repetition.
    std::vector<Move> moves{ make_move(SQ_E2, SQ_E4), make_move(SQ_D7, SQ_D6),
        make_move(SQ_D1, SQ_G4), make_move(SQ_G8, SQ_F6),
        make_move(SQ_G4, SQ_D1), make_move(SQ_F6, SQ_G8),
        make_move(SQ_D1, SQ_G4) };
    std::vector<Node*> nodes{};
    Node* node = game->Root();
    for (Move move : moves)
    {
        nodes.push_back(new Node(move, 1.f));
        node = node->firstChild = nodes.back();
    }

    // Apply the moves and evaluate the 2-repetition as a draw using the
    // starting position as the search root.
    {
        SelfPlayGame searchRoot = *game;
        for (int i = 0; i < moves.size(); i++)
        {
            searchRoot.ApplyMoveWithRoot(moves[i], nodes[i]);
        }

        SelfPlayState state = SelfPlayState::Working;
        PredictionCacheChunk* cacheStore = nullptr;
        const float value = searchRoot.ExpandAndEvaluate(state, cacheStore);
        EXPECT_EQ(value, CHESSCOACH_VALUE_DRAW);
    }

    // Apply 6 moves, snap off a search root, then evaluate the final
    // position as a non-draw since it's not a 2-repetition past the
    // search root.
    {
        SelfPlayGame progress = *game;
        for (int i = 0; i < 6; i++)
        {
            progress.ApplyMoveWithRoot(moves[i], nodes[i]);
        }

        SelfPlayGame searchRoot = progress;
        for (int i = 6; i < moves.size(); i++)
        {
            searchRoot.ApplyMoveWithRoot(moves[i], nodes[i]);
        }

        SelfPlayState state = SelfPlayState::Working;
        PredictionCacheChunk* cacheStore = nullptr;
        const float value = searchRoot.ExpandAndEvaluate(state, cacheStore);
        EXPECT_NE(value, CHESSCOACH_VALUE_DRAW);
        EXPECT_TRUE(std::isnan(value)); // A non-terminal position requires a network evaluation.
    }

    game->PruneAll();
}