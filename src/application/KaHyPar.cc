/***************************************************************************
 *  Copyright (C) 2014 Sebastian Schlag <sebastian.schlag@kit.edu>
 **************************************************************************/
#include <boost/program_options.hpp>

#include <chrono>
#include <memory>
#include <string>

#include "lib/core/Factory.h"
#include "lib/core/PolicyRegistry.h"
#include "lib/core/StaticDispatcher.h"
#include "lib/core/Typelist.h"
#include "lib/definitions.h"
#include "lib/io/HypergraphIO.h"
#include "lib/io/PartitioningOutput.h"
#include "lib/macros.h"
#include "lib/serializer/SQLPlotToolsSerializer.h"
#include "partition/Configuration.h"
#include "partition/Metrics.h"
#include "partition/Partitioner.h"
#include "partition/coarsening/FullHeavyEdgeCoarsener.h"
#include "partition/coarsening/HeuristicHeavyEdgeCoarsener.h"
#include "partition/coarsening/HyperedgeCoarsener.h"
#include "partition/coarsening/HyperedgeRatingPolicies.h"
#include "partition/coarsening/ICoarsener.h"
#include "partition/coarsening/LazyUpdateHeavyEdgeCoarsener.h"
#include "partition/coarsening/Rater.h"
#include "partition/refinement/FMFactoryExecutor.h"
#include "partition/refinement/HyperedgeFMRefiner.h"
#include "partition/refinement/IRefiner.h"
#include "partition/refinement/KWayFMRefiner.h"
#include "partition/refinement/LPRefiner.h"
#include "partition/refinement/MaxGainNodeKWayFMRefiner.h"
#include "partition/refinement/TwoWayFMRefiner.h"
#include "partition/refinement/policies/FMQueueCloggingPolicies.h"
#include "partition/refinement/policies/FMStopPolicies.h"
#include "tools/RandomFunctions.h"

namespace po = boost::program_options;

using core::StaticDispatcher;
using core::Typelist;
using core::NullType;
using core::PolicyRegistry;
using core::NullPolicy;
using core::Factory;

using partition::Rater;
using partition::ICoarsener;
using partition::IRefiner;
using partition::HeuristicHeavyEdgeCoarsener;
using partition::FullHeavyEdgeCoarsener;
using partition::LazyUpdateHeavyEdgeCoarsener;
using partition::Partitioner;
using partition::RandomRatingWins;
using partition::InitialPartitioner;
using partition::Configuration;
using partition::HyperedgeCoarsener;
using partition::EdgeWeightDivMultPinWeight;
using partition::FMFactoryExecutor;
using partition::KFMFactoryExecutor;
using partition::TwoWayFMRefiner;
using partition::HyperedgeFMRefiner;
using partition::KWayFMRefiner;
using partition::MaxGainNodeKWayFMRefiner;
using partition::LPRefiner;
using partition::StoppingPolicy;
using partition::NumberOfFruitlessMovesStopsSearch;
using partition::RandomWalkModelStopsSearch;
using partition::nGPRandomWalkStopsSearch;
using partition::CloggingPolicy;
using partition::OnlyRemoveIfBothQueuesClogged;
using partition::RemoveOnlyTheCloggingEntry;
using partition::DoNotRemoveAnyCloggingEntriesAndResetEligiblity;
using partition::RefinerParameters;
using partition::CoarseningAlgorithm;
using partition::RefinementAlgorithm;
using partition::RefinementStoppingRule;

using serializer::SQLPlotToolsSerializer;

using defs::Hypergraph;
using defs::HypernodeID;
using defs::HypernodeWeight;
using defs::HyperedgeID;
using defs::HyperedgeIndexVector;
using defs::HyperedgeVector;
using defs::HyperedgeWeightVector;
using defs::HypernodeWeightVector;
using defs::HighResClockTimepoint;

void configurePartitionerFromCommandLineInput(Configuration& config, const po::variables_map& vm) {
  if (vm.count("hgr") && vm.count("e") && vm.count("k")) {
    config.partition.graph_filename = vm["hgr"].as<std::string>();
    config.partition.k = vm["k"].as<PartitionID>();

    config.partition.coarse_graph_filename = std::string("/tmp/PID_")
                                             + std::to_string(getpid())
                                             + std::string("_coarse_")
                                             + config.partition.graph_filename.substr(
      config.partition.graph_filename.find_last_of("/")
      + 1);
    config.partition.graph_partition_filename = config.partition.graph_filename + ".part."
                                                + std::to_string(config.partition.k)
                                                + ".KaHyPar";
    config.partition.coarse_graph_partition_filename = config.partition.coarse_graph_filename
                                                       + ".part."
                                                       + std::to_string(config.partition.k);
    config.partition.epsilon = vm["e"].as<double>();

    if (vm.count("seed")) {
      config.partition.seed = vm["seed"].as<int>();
    }
    if (vm.count("nruns")) {
      config.partition.initial_partitioning_attempts = vm["nruns"].as<int>();
    }
    if (vm.count("part")) {
      if (vm["part"].as<std::string>() == "hMetis") {
        config.partition.initial_partitioner = InitialPartitioner::hMetis;
      } else if (vm["part"].as<std::string>() == "PaToH") {
        config.partition.initial_partitioner = InitialPartitioner::PaToH;
      }
    }
    if (vm.count("part-path")) {
      config.partition.initial_partitioner_path = vm["part-path"].as<std::string>();
    } else {
      switch (config.partition.initial_partitioner) {
        case InitialPartitioner::hMetis:
          config.partition.initial_partitioner_path = "/software/hmetis-2.0pre1/Linux-x86_64/hmetis2.0pre1";
          break;
        case InitialPartitioner::PaToH:
          config.partition.initial_partitioner_path = "/software/patoh-Linux-x86_64/Linux-x86_64/patoh";
          break;
      }
    }
    if (vm.count("vcycles")) {
      config.partition.global_search_iterations = vm["vcycles"].as<int>();
    }
    if (vm.count("cmaxnet")) {
      config.partition.hyperedge_size_threshold = vm["cmaxnet"].as<HyperedgeID>();
      if (config.partition.hyperedge_size_threshold == -1) {
        config.partition.hyperedge_size_threshold = std::numeric_limits<HyperedgeID>::max();
      }
    }
    if (vm.count("ctype")) {
      if (vm["ctype"].as<std::string>() == "heavy_full") {
        config.partition.coarsening_algorithm = CoarseningAlgorithm::heavy_full;
      } else if (vm["ctype"].as<std::string>() == "heavy_partial") {
        config.partition.coarsening_algorithm = CoarseningAlgorithm::heavy_partial;
      } else if (vm["ctype"].as<std::string>() == "heavy_lazy") {
        config.partition.coarsening_algorithm = CoarseningAlgorithm::heavy_lazy;
      } else if (vm["ctype"].as<std::string>() == "hyperedge") {
        config.partition.coarsening_algorithm = CoarseningAlgorithm::hyperedge;
      } else {
        std::cout << "Illegal ctype option! Exiting..." << std::endl;
        exit(0);
      }
    }
    if (vm.count("s")) {
      config.coarsening.max_allowed_weight_multiplier = vm["s"].as<double>();
    }
    if (vm.count("t")) {
      config.coarsening.contraction_limit_multiplier = vm["t"].as<HypernodeID>();
    }
    if (vm.count("stopFM")) {
      if (vm["stopFM"].as<std::string>() == "simple") {
        config.fm_local_search.stopping_rule = RefinementStoppingRule::simple;
        config.her_fm.stopping_rule = RefinementStoppingRule::simple;
      } else if (vm["stopFM"].as<std::string>() == "adaptive1") {
        config.fm_local_search.stopping_rule = RefinementStoppingRule::adaptive1;
        config.her_fm.stopping_rule = RefinementStoppingRule::adaptive1;
      } else if (vm["stopFM"].as<std::string>() == "adaptive2") {
        config.fm_local_search.stopping_rule = RefinementStoppingRule::adaptive2;
        config.her_fm.stopping_rule = RefinementStoppingRule::adaptive2;
      } else {
        std::cout << "Illegal stopFM option! Exiting..." << std::endl;
        exit(0);
      }
    }
    if (vm.count("FMreps")) {
      config.fm_local_search.num_repetitions = vm["FMreps"].as<int>();
      config.her_fm.num_repetitions = vm["FMreps"].as<int>();
      if (config.fm_local_search.num_repetitions == -1) {
        config.fm_local_search.num_repetitions = std::numeric_limits<int>::max();
        config.her_fm.num_repetitions = std::numeric_limits<int>::max();
      }
    }
    if (vm.count("i")) {
      config.fm_local_search.max_number_of_fruitless_moves = vm["i"].as<int>();
      config.her_fm.max_number_of_fruitless_moves = vm["i"].as<int>();
    }
    if (vm.count("alpha")) {
      config.fm_local_search.alpha = vm["alpha"].as<double>();
      if (config.fm_local_search.alpha == -1) {
        config.fm_local_search.alpha = std::numeric_limits<double>::max();
      }
    }
    if (vm.count("verbose")) {
      config.partition.verbose_output = vm["verbose"].as<bool>();
    }
    if (vm.count("init-remove-hes")) {
      config.partition.initial_parallel_he_removal = vm["init-remove-hes"].as<bool>();
    }
    if (vm.count("lp_refiner_max_iterations")) {
      config.lp_refiner.max_number_iterations = vm["lp_refiner_max_iterations"].as<int>();
    }
    if (vm.count("rtype")) {
      if (vm["rtype"].as<std::string>() == "twoway_fm") {
        config.partition.refinement_algorithm = RefinementAlgorithm::twoway_fm;
      } else if (vm["rtype"].as<std::string>() == "kway_fm") {
        config.partition.refinement_algorithm = RefinementAlgorithm::kway_fm;
      } else if (vm["rtype"].as<std::string>() == "kway_fm_maxgain") {
        config.partition.refinement_algorithm = RefinementAlgorithm::kway_fm_maxgain;
      } else if (vm["rtype"].as<std::string>() == "hyperedge") {
        config.partition.refinement_algorithm = RefinementAlgorithm::hyperedge;
      } else if (vm["rtype"].as<std::string>() == "label_propagation") {
        config.partition.refinement_algorithm = RefinementAlgorithm::label_propagation;
      } else {
        std::cout << "Illegal stopFM option! Exiting..." << std::endl;
        exit(0);
      }
    }
  } else {
    std::cout << "Parameter error! Exiting..." << std::endl;
    exit(0);
  }
}

void setDefaults(Configuration& config) {
  config.partition.k = 2;
  config.partition.epsilon = 0.05;
  config.partition.seed = -1;
  config.partition.initial_partitioning_attempts = 10;
  config.partition.global_search_iterations = 10;
  config.partition.hyperedge_size_threshold = -1;
  config.partition.coarsening_algorithm = CoarseningAlgorithm::heavy_full;
  config.partition.refinement_algorithm = RefinementAlgorithm::kway_fm;
  config.coarsening.contraction_limit_multiplier = 160;
  config.coarsening.max_allowed_weight_multiplier = 3.5;
  config.coarsening.contraction_limit =
    config.coarsening.contraction_limit_multiplier * config.partition.k;
  config.coarsening.hypernode_weight_fraction = config.coarsening.max_allowed_weight_multiplier
                                                / config.coarsening.contraction_limit;
  config.fm_local_search.stopping_rule = RefinementStoppingRule::simple;
  config.fm_local_search.num_repetitions = -1;
  config.fm_local_search.max_number_of_fruitless_moves = 150;
  config.fm_local_search.alpha = 8;
  config.her_fm.stopping_rule = RefinementStoppingRule::simple;
  config.her_fm.num_repetitions = 1;
  config.her_fm.max_number_of_fruitless_moves = 10;
  config.lp_refiner.max_number_iterations = 3;
}

struct CoarsenerFactoryParameters {
  CoarsenerFactoryParameters(Hypergraph& hgr, Configuration& conf) :
    hypergraph(hgr),
    config(conf) { }
  Hypergraph& hypergraph;
  Configuration& config;
};

int main(int argc, char* argv[]) {
  using RandomWinsRater = Rater<defs::RatingType, RandomRatingWins>;
  using RandomWinsHeuristicCoarsener = HeuristicHeavyEdgeCoarsener<RandomWinsRater>;
  using RandomWinsFullCoarsener = FullHeavyEdgeCoarsener<RandomWinsRater>;
  using RandomWinsLazyUpdateCoarsener = LazyUpdateHeavyEdgeCoarsener<RandomWinsRater>;
  using HyperedgeCoarsener = HyperedgeCoarsener<EdgeWeightDivMultPinWeight>;
  using TwoWayFMFactoryExecutor = KFMFactoryExecutor<TwoWayFMRefiner>;
  using HyperedgeFMFactoryExecutor = FMFactoryExecutor<HyperedgeFMRefiner>;
  using KWayFMFactoryExecutor = KFMFactoryExecutor<KWayFMRefiner>;
  using MaxGainNodeKWayFMFactoryExecutor = KFMFactoryExecutor<MaxGainNodeKWayFMRefiner>;
  using TwoWayFMFactoryDispatcher = StaticDispatcher<TwoWayFMFactoryExecutor,
                                                     PolicyBase,
                                                     Typelist<NumberOfFruitlessMovesStopsSearch,
                                                              RandomWalkModelStopsSearch,
                                                              nGPRandomWalkStopsSearch>,
                                                     PolicyBase,
                                                     Typelist<NullPolicy>,
                                                     IRefiner*>;
  using HyperedgeFMFactoryDispatcher = StaticDispatcher<HyperedgeFMFactoryExecutor,
                                                        PolicyBase,
                                                        Typelist<NumberOfFruitlessMovesStopsSearch,
                                                                 RandomWalkModelStopsSearch,
                                                                 nGPRandomWalkStopsSearch>,
                                                        PolicyBase,
                                                        Typelist<OnlyRemoveIfBothQueuesClogged>,
                                                        IRefiner*>;
  using KWayFMFactoryDispatcher = StaticDispatcher<KWayFMFactoryExecutor,
                                                   PolicyBase,
                                                   Typelist<NumberOfFruitlessMovesStopsSearch,
                                                            RandomWalkModelStopsSearch,
                                                            nGPRandomWalkStopsSearch>,
                                                   PolicyBase,
                                                   Typelist<NullPolicy>,
                                                   IRefiner*>;
  using MaxGainNodeKWayFMFactoryDispatcher = StaticDispatcher<MaxGainNodeKWayFMFactoryExecutor,
                                                              PolicyBase,
                                                              Typelist<NumberOfFruitlessMovesStopsSearch,
                                                                       RandomWalkModelStopsSearch,
                                                                       nGPRandomWalkStopsSearch>,
                                                              PolicyBase,
                                                              Typelist<NullPolicy>,
                                                              IRefiner*>;
  using CoarsenerFactory = Factory<ICoarsener, CoarseningAlgorithm,
                                   ICoarsener* (*)(CoarsenerFactoryParameters&),
                                   CoarsenerFactoryParameters>;

  PolicyRegistry<RefinementStoppingRule>::getInstance().registerPolicy(
    RefinementStoppingRule::simple, new NumberOfFruitlessMovesStopsSearch());
  PolicyRegistry<RefinementStoppingRule>::getInstance().registerPolicy(
    RefinementStoppingRule::adaptive1, new RandomWalkModelStopsSearch());
  PolicyRegistry<RefinementStoppingRule>::getInstance().registerPolicy(
    RefinementStoppingRule::adaptive2, new nGPRandomWalkStopsSearch());

  CoarsenerFactory::getInstance().registerObject(
    CoarseningAlgorithm::hyperedge,
    [](CoarsenerFactoryParameters& p) -> ICoarsener* {
    return new HyperedgeCoarsener(p.hypergraph, p.config);
  });

  CoarsenerFactory::getInstance().registerObject(
    CoarseningAlgorithm::heavy_partial,
    [](CoarsenerFactoryParameters& p) -> ICoarsener* {
    return new RandomWinsHeuristicCoarsener(p.hypergraph, p.config);
  });

  CoarsenerFactory::getInstance().registerObject(
    CoarseningAlgorithm::heavy_full,
    [](CoarsenerFactoryParameters& p) -> ICoarsener* {
    return new RandomWinsFullCoarsener(p.hypergraph, p.config);
  });

  CoarsenerFactory::getInstance().registerObject(
    CoarseningAlgorithm::heavy_lazy,
    [](CoarsenerFactoryParameters& p) -> ICoarsener* {
    return new RandomWinsLazyUpdateCoarsener(p.hypergraph, p.config);
  });

  po::options_description desc("Allowed options");
  desc.add_options()
    ("help", "show help message")
    ("verbose", po::value<bool>(), "Verbose partitioner output")
    ("hgr", po::value<std::string>(), "Filename of the hypergraph to be partitioned")
    ("k", po::value<PartitionID>(), "Number of partitions")
    ("e", po::value<double>(), "Imbalance parameter epsilon")
    ("seed", po::value<int>(), "Seed for random number generator")
    ("init-remove-hes", po::value<bool>(), "Initially remove parallel hyperedges before partitioning")
    ("nruns", po::value<int>(),
    "# initial partition trials, the final bisection corresponds to the one with the smallest cut")
    ("part", po::value<std::string>(), "Initial Partitioner: hMetis (default), PaToH")
    ("part-path", po::value<std::string>(), "Path to Initial Partitioner Binary")
    ("vcycles", po::value<int>(), "# v-cycle iterations")
    ("cmaxnet", po::value<HyperedgeID>(), "Any hyperedges larger than cmaxnet are removed from the hypergraph before partition (disable:-1 (default))")
    ("ctype", po::value<std::string>(), "Coarsening: Scheme to be used: heavy_full (default), heavy_heuristic, heavy_lazy, hyperedge")
    ("s", po::value<double>(),
    "Coarsening: The maximum weight of a hypernode in the coarsest is:(s * w(Graph)) / (t * k)")
    ("t", po::value<HypernodeID>(), "Coarsening: Coarsening stops when there are no more than t * k hypernodes left")
    ("rtype", po::value<std::string>(), "Refinement: 2way_fm (default for k=2), her_fm, max_gain_kfm, kfm, lp_refiner")
    ("lp_refiner_max_iterations", po::value<int>(), "Refinement: maximum number of iterations for label propagation based refinement")
    ("stopFM", po::value<std::string>(), "2-Way-FM | HER-FM: Stopping rule \n adaptive1: new implementation based on nGP \n adaptive2: original nGP implementation \n simple: threshold based")
    ("FMreps", po::value<int>(), "2-Way-FM | HER-FM: max. # of local search repetitions on each level (default:1, no limit:-1)")
    ("i", po::value<int>(), "2-Way-FM | HER-FM: max. # fruitless moves before stopping local search (simple)")
    ("alpha", po::value<double>(), "2-Way-FM: Random Walk stop alpha (adaptive) (infinity: -1)")
    ("file", po::value<std::string>(), "filename of result file");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if (vm.count("help")) {
    std::cout << desc << std::endl;
    return 0;
  }

  std::string result_file;
  if (vm.count("file")) {
    result_file = vm["file"].as<std::string>();
  }

  Configuration config;
  setDefaults(config);
  configurePartitionerFromCommandLineInput(config, vm);

  Randomize::setSeed(config.partition.seed);

  HypernodeID num_hypernodes;
  HyperedgeID num_hyperedges;
  HyperedgeIndexVector index_vector;
  HyperedgeVector edge_vector;

  io::readHypergraphFile(config.partition.graph_filename, num_hypernodes, num_hyperedges,
                         index_vector, edge_vector);
  Hypergraph hypergraph(num_hypernodes, num_hyperedges, index_vector, edge_vector,
                        config.partition.k);

  config.partition.total_graph_weight = hypergraph.totalWeight();

  config.coarsening.contraction_limit = config.coarsening.contraction_limit_multiplier
                                        * config.partition.k;

  config.coarsening.hypernode_weight_fraction = config.coarsening.max_allowed_weight_multiplier
                                                / config.coarsening.contraction_limit;

  config.partition.max_part_weight = (1 + config.partition.epsilon)
                                     * ceil(config.partition.total_graph_weight /
                                            static_cast<double>(config.partition.k));


  config.coarsening.max_allowed_node_weight = config.coarsening.hypernode_weight_fraction *
                                              config.partition.total_graph_weight;
  config.fm_local_search.beta = log(num_hypernodes);

  // We use hMetis-RB as initial partitioner. If called to partition a graph into k parts
  // with an UBfactor of b, the maximal allowed partition size will be 0.5+(b/100)^(log2(k)) n.
  // In order to provide a balanced initial partitioning, we determine the UBfactor such that
  // the maximal allowed partiton size corresponds to our upper bound i.e.
  // (1+epsilon) * ceil(total_weight / k).
  double exp = 1.0 / log2(config.partition.k);
  config.partition.hmetis_ub_factor =
    50.0 * (2 * pow((1 + config.partition.epsilon), exp)
            * pow(ceil(static_cast<double>(config.partition.total_graph_weight)
                       / config.partition.k) / config.partition.total_graph_weight, exp) - 1);

  io::printPartitionerConfiguration(config);
  io::printHypergraphInfo(hypergraph, config.partition.graph_filename.substr(
                            config.partition.graph_filename.find_last_of("/") + 1));

#ifdef GATHER_STATS
  LOG("*******************************");
  LOG("***** GATHER_STATS ACTIVE *****");
  LOG("*******************************");
#endif

  Partitioner partitioner(config);
  CoarsenerFactoryParameters coarsener_parameters(hypergraph, config);

  std::unique_ptr<ICoarsener> coarsener(
    CoarsenerFactory::getInstance().createObject(
      config.partition.coarsening_algorithm, coarsener_parameters));

  std::unique_ptr<CloggingPolicy> clogging_policy(new OnlyRemoveIfBothQueuesClogged());
  std::unique_ptr<NullPolicy> null_policy(new NullPolicy());
  RefinerParameters refiner_parameters(hypergraph, config);
  std::unique_ptr<IRefiner> refiner(nullptr);

  switch (config.partition.refinement_algorithm) {
    case RefinementAlgorithm::twoway_fm: {
        TwoWayFMFactoryExecutor exec;
        refiner.reset(TwoWayFMFactoryDispatcher::go(
                        PolicyRegistry<RefinementStoppingRule>::getInstance().getPolicy(
                          config.fm_local_search.stopping_rule),
                        *(null_policy.get()),
                        exec, refiner_parameters));
      }
      break;
    case RefinementAlgorithm::kway_fm_maxgain: {
        MaxGainNodeKWayFMFactoryExecutor exec;
        refiner.reset(MaxGainNodeKWayFMFactoryDispatcher::go(
                        PolicyRegistry<RefinementStoppingRule>::getInstance().getPolicy(
                          config.fm_local_search.stopping_rule),
                        *(null_policy.get()),
                        exec, refiner_parameters));
      }
      break;
    case RefinementAlgorithm::kway_fm: {
        KWayFMFactoryExecutor exec;
        refiner.reset(KWayFMFactoryDispatcher::go(
                        PolicyRegistry<RefinementStoppingRule>::getInstance().getPolicy(
                          config.fm_local_search.stopping_rule),
                        *(null_policy.get()),
                        exec, refiner_parameters));
      }
      break;
    case RefinementAlgorithm::label_propagation:
      refiner.reset(new LPRefiner(hypergraph, config));
      break;
    case RefinementAlgorithm::hyperedge: {
        HyperedgeFMFactoryExecutor exec;
        refiner.reset(HyperedgeFMFactoryDispatcher::go(
                        PolicyRegistry<RefinementStoppingRule>::getInstance().getPolicy(
                          config.her_fm.stopping_rule),
                        *(clogging_policy.get()),
                        exec, refiner_parameters));
      }
  }

  HighResClockTimepoint start;
  HighResClockTimepoint end;

  start = std::chrono::high_resolution_clock::now();
  partitioner.performDirectKwayPartitioning(hypergraph, *coarsener, *refiner);
  end = std::chrono::high_resolution_clock::now();

#ifndef NDEBUG
  for (const HyperedgeID he : hypergraph.edges()) {
    ASSERT([&]() -> bool {
      HypernodeID num_pins = 0;
      for (PartitionID i = 0; i < config.partition.k; ++i) {
        num_pins += hypergraph.pinCountInPart(he, i);
      }
      return num_pins == hypergraph.edgeSize(he);
    } (),
           "Incorrect calculation of pin counts");
  }
#endif

  std::chrono::duration<double> elapsed_seconds = end - start;

  io::printPartitioningStatistics(partitioner, *coarsener, *refiner);
  io::printPartitioningResults(hypergraph, elapsed_seconds, partitioner.timings());
  io::writePartitionFile(hypergraph, config.partition.graph_partition_filename);

  std::remove(config.partition.coarse_graph_filename.c_str());
  std::remove(config.partition.coarse_graph_partition_filename.c_str());

  SQLPlotToolsSerializer::serialize(config, hypergraph, partitioner, *coarsener, *refiner,
                                    elapsed_seconds, partitioner.timings(), result_file);
  return 0;
}
