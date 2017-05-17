#include <iostream>
#include <numeric>
#include <algorithm>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <typeinfo>

#include <hpx/hpx.hpp>
#include <hpx/hpx_init.hpp>

#include <boost/serialization/access.hpp>

#include "DimacsParser.hpp"
#include "BitGraph.hpp"
#include "BitSet.hpp"

#include "bnb/bnb-seq.hpp"

// 64 bit words
// 8 words covers 400 vertices
// 13 words covers 800 vertices
// Later we can specialise this at compile time
#define NWORDS 8

// Order a graphFromFile and return an ordered graph alongside a map to invert
// the vertex numbering at the end.
template<unsigned n_words_>
auto orderGraphFromFile(const dimacs::GraphFromFile & g, std::map<int,int> & inv) -> BitGraph<n_words_> {
  std::vector<int> order(g.first);
  std::iota(order.begin(), order.end(), 0);

  // Order by degree, tie break on number
  std::vector<int> degrees;
  std::transform(order.begin(), order.end(), std::back_inserter(degrees),
                 [&] (int v) { return g.second.find(v)->second.size(); });

  std::sort(order.begin(), order.end(),
            [&] (int a, int b) { return ! (degrees[a] < degrees[b] || (degrees[a] == degrees[b] && a > b)); });


  // Construct a new graph with this new ordering
  BitGraph<n_words_> graph;
  graph.resize(g.first);

  for (unsigned i = 0 ; i < g.first ; ++i)
    for (unsigned j = 0 ; j < g.first ; ++j)
      if (g.second.find(order[i])->second.count(order[j]))
        graph.add_edge(i, j);

  // Create inv map (maybe just return order?)
  for (int i = 0; i < order.size(); i++) {
    inv[i] = order[i];
  }

  return graph;
}

template<unsigned n_words_>
auto colour_class_order(const BitGraph<n_words_> & graph,
                        const BitSet<n_words_> & p,
                        std::array<unsigned, n_words_ * bits_per_word> & p_order,
                        std::array<unsigned, n_words_ * bits_per_word> & p_bounds) -> void {
  BitSet<n_words_> p_left = p; // not coloured yet
  unsigned colour = 0;         // current colour
  unsigned i = 0;              // position in p_bounds

  // while we've things left to colour
  while (! p_left.empty()) {
    // next colour
    ++colour;
    // things that can still be given this colour
    BitSet<n_words_> q = p_left;

    // while we can still give something this colour
    while (! q.empty()) {
      // first thing we can colour
      int v = q.first_set_bit();
      p_left.unset(v);
      q.unset(v);

      // can't give anything adjacent to this the same colour
      graph.intersect_with_row_complement(v, q);

      // record in result
      p_bounds[i] = colour;
      p_order[i] = v;
      ++i;
    }
  }
}

// Main Maxclique B&B Functions
// Probably needs a copy constructor
struct MCSol {
  std::vector<int> members;
  int colours;

  template <class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & members;
    ar & colours;
  }
};

using MCNode = hpx::util::tuple<MCSol, int, BitSet<NWORDS> >;

std::vector<MCNode> generateChoices(const BitGraph<NWORDS> & graph, const MCNode & n) {
  std::array<unsigned, NWORDS * bits_per_word> p_order;
  std::array<unsigned, NWORDS * bits_per_word> colourClass;
  auto p = hpx::util::get<2>(n);

  colour_class_order(graph, p, p_order, colourClass);

  std::vector<MCNode> res;
  res.reserve(p.popcount());

  for (int v = p.popcount() - 1 ; v >= 0 ; --v) {
    auto childSol = hpx::util::get<0>(n);
    childSol.members.reserve(graph.size());
    childSol.members.push_back(p_order[v]);
    // -1 since we have effectively "taken" one colour class in the child
    childSol.colours = colourClass[v] - 1;

    auto childBnd = hpx::util::get<1>(n) + 1;

    auto childCands = p;
    graph.intersect_with_row(p_order[v], childCands);

    res.push_back(hpx::util::make_tuple(childSol, childBnd, childCands));

    p.unset(p_order[v]);
  }

  return res;
}

int upperBound(const BitGraph<NWORDS> & space, const MCNode & n) {
  return hpx::util::get<1>(n) + hpx::util::get<0>(n).colours;
}


int hpx_main(boost::program_options::variables_map & opts) {
  auto inputFile = opts["input-file"].as<std::string>();
  if (inputFile.empty()) {
    hpx::finalize();
    return EXIT_FAILURE;
  }

  const std::vector<std::string> skeletonTypes = {"seq"};

  auto skeletonType = opts["skeleton-type"].as<std::string>();
  auto found = std::find(std::begin(skeletonTypes), std::end(skeletonTypes), skeletonType);
  if (found == std::end(skeletonTypes)) {
    std::cout << "Invalid skeleton type option. Should be: seq, par or dist" << std::endl;
    hpx::finalize();
    return EXIT_FAILURE;
  }

  auto gFile = dimacs::read_dimacs(inputFile);

  // Order the graph (keep a hold of the map)
  std::map<int, int> invMap;
  auto graph = orderGraphFromFile<NWORDS>(gFile, invMap);

  auto spawnDepth = opts["spawn-depth"].as<std::uint64_t>();

  auto start_time = std::chrono::steady_clock::now();

  // Initialise Root Node
  MCSol mcsol;
  mcsol.members.reserve(graph.size());
  mcsol.colours = 0;

  BitSet<NWORDS> cands;
  cands.resize(graph.size());
  cands.set_all();
  auto root = hpx::util::make_tuple(mcsol, 0, cands);

  auto sol = root;
  if (skeletonType == "seq") {
    sol = skeletons::BnB::Seq::search<BitGraph<NWORDS>, MCSol, int, BitSet<NWORDS> >
      (graph, root, generateChoices, upperBound);
  }

  auto overall_time = std::chrono::duration_cast<std::chrono::milliseconds>
    (std::chrono::steady_clock::now() - start_time);

  auto maxCliqueSize = hpx::util::get<1>(sol);

  std::cout << "MaxClique Size = " << maxCliqueSize << std::endl;
  std::cout << "cpu = " << overall_time.count() << std::endl;
  std::cout << "Exapnds = " << skeletons::BnB::Seq::numExpands << std::endl;

  return hpx::finalize();
}

int main (int argc, char* argv[]) {
  boost::program_options::options_description
    desc_commandline("Usage: " HPX_APPLICATION_STRING " [options]");

  desc_commandline.add_options()
    ( "spawn-depth,d",
      boost::program_options::value<std::uint64_t>()->default_value(0),
      "Depth in the tree to spawn at"
      )
    ( "input-file,f",
      boost::program_options::value<std::string>(),
      "DIMACS formatted input graph"
      )
    ( "skeleton-type",
      boost::program_options::value<std::string>()->default_value("seq"),
      "Which skeleton to use"
      );

  return hpx::init(desc_commandline, argc, argv);
}

