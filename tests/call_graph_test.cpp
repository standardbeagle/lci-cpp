#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include <lci/analysis/call_graph.h>
#include <lci/types.h>

namespace lci {
namespace analysis {
namespace {

// Builds a CallGraph from an explicit adjacency map keyed by SymbolID.
CallGraph make_graph(
    const std::vector<SymbolID>& nodes,
    const absl::flat_hash_map<SymbolID, std::vector<SymbolID>>& edges) {
    CallGraph g;
    g.build(nodes, [&](SymbolID id) -> std::vector<SymbolID> {
        auto it = edges.find(id);
        return it == edges.end() ? std::vector<SymbolID>{} : it->second;
    });
    return g;
}

// Chain: top -> mid -> leaf. leaf is reached by {mid, top} = 2; mid by {top} = 1.
TEST(CallGraph, ReachOnChain) {
    auto g = make_graph({1, 2, 3}, {{1, {2}}, {2, {3}}});  // 1=top,2=mid,3=leaf
    auto reach = g.incoming_reach();
    ASSERT_EQ(reach.size(), 3u);
    // idx order matches node order: 0=id1(top), 1=id2(mid), 2=id3(leaf)
    EXPECT_EQ(reach[0], 0);  // top: nobody calls it
    EXPECT_EQ(reach[1], 1);  // mid: top
    EXPECT_EQ(reach[2], 2);  // leaf: top, mid
    EXPECT_TRUE(g.cycles().empty());
}

// Diamond: a->b, a->c, b->d, c->d. d reached by {a,b,c}=3 despite two paths.
TEST(CallGraph, ReachDeduplicatesDiamond) {
    auto g = make_graph({1, 2, 3, 4},
                        {{1, {2, 3}}, {2, {4}}, {3, {4}}});
    auto reach = g.incoming_reach();
    // idx: 0=a,1=b,2=c,3=d
    EXPECT_EQ(reach[3], 3);  // d reached by a,b,c — counted once each
    EXPECT_EQ(reach[1], 1);  // b reached by a
    EXPECT_EQ(reach[2], 1);  // c reached by a
    EXPECT_EQ(reach[0], 0);  // a
}

// Cycle: a<->b, both call shared. Within-SCC members reach each other.
TEST(CallGraph, DetectsCycleAndReachThroughScc) {
    auto g = make_graph({1, 2, 3}, {{1, {2}}, {2, {1, 3}}});  // 1<->2, 2->3
    auto cyc = g.cycles();
    ASSERT_EQ(cyc.size(), 1u);
    EXPECT_EQ(cyc[0].size(), 2u);  // {a,b}

    auto reach = g.incoming_reach();
    // idx: 0=a,1=b,2=shared. a&b in one SCC: each reached by the other (1) -> 1.
    EXPECT_EQ(reach[0], 1);
    EXPECT_EQ(reach[1], 1);
    EXPECT_EQ(reach[2], 2);  // shared reached by a and b
}

TEST(CallGraph, SelfLoopIsACycle) {
    auto g = make_graph({1}, {{1, {1}}});
    EXPECT_EQ(g.cycles().size(), 1u);
}

TEST(CallGraph, IgnoresEdgesOutsideNodeSet) {
    // 1 calls 99 (not in node set) and 2; only the 1->2 edge survives.
    auto g = make_graph({1, 2}, {{1, {99, 2}}});
    auto reach = g.incoming_reach();
    EXPECT_EQ(reach[1], 1);  // node 2 reached by node 1
    EXPECT_EQ(reach[0], 0);
}

TEST(CallGraph, EmptyGraph) {
    CallGraph g;
    g.build({}, [](SymbolID) { return std::vector<SymbolID>{}; });
    EXPECT_EQ(g.node_count(), 0);
    EXPECT_TRUE(g.incoming_reach().empty());
    EXPECT_TRUE(g.cycles().empty());
    double q = -1;
    EXPECT_TRUE(g.louvain_communities(q).empty());
    EXPECT_DOUBLE_EQ(q, 0.0);
}

// Two triangles (1-2-3 and 4-5-6) joined by a single bridge edge 3->4 must
// split into two communities with positive modularity.
TEST(CallGraph, LouvainSplitsTwoCliques) {
    auto g = make_graph(
        {1, 2, 3, 4, 5, 6},
        {{1, {2, 3}}, {2, {3, 1}}, {3, {1, 2, 4}},  // triangle A + bridge
         {4, {5, 6}}, {5, {6, 4}}, {6, {4, 5}}});    // triangle B
    double q = 0;
    auto comm = g.louvain_communities(q);
    ASSERT_EQ(comm.size(), 6u);
    // Nodes 1,2,3 (idx 0,1,2) share a community; 4,5,6 (idx 3,4,5) share another.
    EXPECT_EQ(comm[0], comm[1]);
    EXPECT_EQ(comm[1], comm[2]);
    EXPECT_EQ(comm[3], comm[4]);
    EXPECT_EQ(comm[4], comm[5]);
    EXPECT_NE(comm[0], comm[3]);
    EXPECT_GT(q, 0.3);  // clear two-community structure
}

// Determinism: same graph, same labels + modularity across runs.
TEST(CallGraph, LouvainDeterministic) {
    absl::flat_hash_map<SymbolID, std::vector<SymbolID>> e = {
        {1, {2, 3}}, {2, {3, 1}}, {3, {1, 2, 4}}, {4, {5, 6}},
        {5, {6, 4}}, {6, {4, 5}}};
    double q1 = 0, q2 = 0;
    auto a = make_graph({1, 2, 3, 4, 5, 6}, e).louvain_communities(q1);
    auto b = make_graph({1, 2, 3, 4, 5, 6}, e).louvain_communities(q2);
    EXPECT_EQ(a, b);
    EXPECT_DOUBLE_EQ(q1, q2);
}

}  // namespace
}  // namespace analysis
}  // namespace lci
