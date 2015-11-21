/*
 * GreedHypergraphGrowing.h
 *
 *  Created on: 15.11.2015
 *      Author: theuer
 */

#ifndef SRC_PARTITION_INITIAL_PARTITIONING_GREEDYHYPERGRAPHGROWINGINITIALPARTITIONER_H_
#define SRC_PARTITION_INITIAL_PARTITIONING_GREEDYHYPERGRAPHGROWINGINITIALPARTITIONER_H_

#include "lib/datastructure/FastResetBitVector.h"
#include "lib/datastructure/KWayPriorityQueue.h"
#include "lib/definitions.h"
#include "partition/initial_partitioning/IInitialPartitioner.h"
#include "partition/initial_partitioning/InitialPartitionerBase.h"
#include "partition/initial_partitioning/policies/GainComputationPolicy.h"
#include "partition/initial_partitioning/policies/GreedyQueueSelectionPolicy.h"
#include "partition/initial_partitioning/policies/StartNodeSelectionPolicy.h"
#include "tools/RandomFunctions.h"

using defs::HypernodeWeight;
using partition::StartNodeSelectionPolicy;
using partition::GainComputationPolicy;
using partition::GreedyQueueSelectionPolicy;
using datastructure::KWayPriorityQueue;

using Gain = HyperedgeWeight;
using KWayRefinementPQ = KWayPriorityQueue<HypernodeID, HyperedgeWeight,
                                           std::numeric_limits<HyperedgeWeight> >;

namespace partition {
template <class StartNodeSelection = StartNodeSelectionPolicy,
          class GainComputation = GainComputationPolicy,
          class QueueSelection = GreedyQueueSelectionPolicy>
class GreedyHypergraphGrowingInitialPartitioner : public IInitialPartitioner,
                                                  private InitialPartitionerBase {
 public:
  GreedyHypergraphGrowingInitialPartitioner(Hypergraph& hypergraph,
                                            Configuration& config) :
    InitialPartitionerBase(hypergraph, config),
    _pq(config.initial_partitioning.k),
    _start_nodes(),
    _visit(_hg.initialNumNodes(), false),
    _hyperedge_in_queue(config.initial_partitioning.k * _hg.initialNumEdges(), false) {
    _pq.initialize(_hg.initialNumNodes());
  }

  ~GreedyHypergraphGrowingInitialPartitioner() { }

 private:
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest, InsertionOfAHypernodeIntoPQ);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              TryingToInsertAHypernodeIntoTheSamePQAsHisCurrentPart);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              ChecksCorrectMaxGainValueAndHypernodeAfterPushingSomeHypernodesIntoPriorityQueue);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              ChecksCorrectGainValueAfterUpdatePriorityQueue);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              ChecksCorrectMaxGainValueAfterDeltaGainUpdate);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              ChecksCorrectHypernodesAndGainValuesInPQAfterAMove);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              ChecksCorrectMaxGainValueAfterDeltaGainUpdateWithUnassignedPartMinusOne);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              DeletesAssignedHypernodesFromPriorityQueue);
  FRIEND_TEST(AGreedyHypergraphGrowingFunctionalityTest,
              CheckIfAllEnabledPQContainsAtLeastOneHypernode);

  void initialPartition() final {
    //Every QueueSelectionPolicy specify its own operating unassigned part. Therefore we only change
	//the unassigned_part variable in this method and reset it at the end to original value.
    const PartitionID unassigned_part = _config.initial_partitioning.unassigned_part;
    _config.initial_partitioning.unassigned_part = QueueSelection::getOperatingUnassignedPart();
    InitialPartitionerBase::resetPartitioning();
    reset();

    // Calculate Startnodes and push them into the queues.
    calculateStartNodes();

    // If the weight of the unassigned part is less than minimum_unassigned_part_weight, than
    // initial partitioning stops.
    HypernodeWeight minimum_unassigned_part_weight = 0;
    if (_config.initial_partitioning.unassigned_part != -1) {
      _pq.disablePart(_config.initial_partitioning.unassigned_part);
      minimum_unassigned_part_weight =
        _config.initial_partitioning.perfect_balance_partition_weight[unassigned_part];
    }

    bool is_upper_bound_released = false;
    // Define a weight bound, which every part has to reach, to avoid very small partitions.
    InitialPartitionerBase::recalculateBalanceConstraints(  /*epsilon*/ 0);

    // TODO(heuer): Would be better if this was initialized to something invalid, since
    // 0 is a _valid_ part id.
    PartitionID current_id = 0;
    while (true) {
      if (_config.initial_partitioning.unassigned_part != -1 &&
          minimum_unassigned_part_weight >
          _hg.partWeight(_config.initial_partitioning.unassigned_part)) {
        break;
      }

      HypernodeID current_hn = kInvalidNode;
	  Gain current_gain = kInvalidGain;

      if (!QueueSelection::nextQueueID(_hg, _config, _pq, current_hn, current_gain, current_id,
    		  is_upper_bound_released)) {
        // Every part is disabled and the upper weight bound is released to finish initial partitioning
        // TODO(heuer): Name of boolean variable is_upper_bound_released seems to be misleading
        if (!is_upper_bound_released) {
          InitialPartitionerBase::recalculateBalanceConstraints(
            _config.initial_partitioning.epsilon);
          is_upper_bound_released = true;
          // TODO(heuer): This also seems unnecessary. Why not just disable the part that became too large?
          for (PartitionID part = 0; part < _config.initial_partitioning.k;
               ++part) {
            if (part != _config.initial_partitioning.unassigned_part && !_pq.isEnabled(part)) {
              if(_pq.size(part) == 0) {
            	  insertUnassignedHypernodeIntoPQ(part);
              }
              else {
            	  _pq.enablePart(part);
              }
            }
          }
          current_id = 0;
          continue;
        } else {
          break;
        }
      }

      ASSERT(current_hn < _hg.numNodes(), "Current Hypernode " << current_hn << " is not a valid hypernode!");
      ASSERT(current_id != -1, "Part " << current_id << " is no valid part!");
      ASSERT(_hg.partID(current_hn) == _config.initial_partitioning.unassigned_part,
             "The current selected hypernode " << current_hn
             << " is already assigned to a part during initial partitioning!");

      if (assignHypernodeToPartition(current_hn, current_id)) {
        ASSERT(_hg.partID(current_hn) == current_id,
               "Assignment of hypernode " << current_hn << " to partition " << current_id
               << " failed!");

        ASSERT([&]() {
            if (_config.initial_partitioning.unassigned_part != -1 &&
                GainComputation::getType() == GainType::fm_gain) {
              _hg.changeNodePart(current_hn, current_id, _config.initial_partitioning.unassigned_part);
              const HyperedgeWeight cut_before = metrics::hyperedgeCut(_hg);
              _hg.changeNodePart(current_hn, _config.initial_partitioning.unassigned_part, current_id);
              return metrics::hyperedgeCut(_hg) == (cut_before - current_gain);
            } else {
              return true;
            }
          } (),
               "Gain calculation of hypernode " << current_hn << " failed!");
        insertAndUpdateNodesAfterMove(current_hn, current_id);
      } else {
        _pq.disablePart(current_id);
      }
    }

    // TODO(heuer): Actually this isn't the correct condition to be checked.
    // Wouldn't it be more precise, if the version of GHG would be checked?
    // Or is this not explicitly stored somewhere? The it should at least
    // be documented that this corresponds to the case, where the upper_bound
    // is used.
    if (_config.initial_partitioning.unassigned_part == -1) {
      for (HypernodeID hn : _hg.nodes()) {
        if (_hg.partID(hn) == -1) {
          const Gain gain0 = GainComputation::calculateGain(_hg, hn, 0);
          const Gain gain1 = GainComputation::calculateGain(_hg, hn, 1);
          if (gain0 > gain1) {
            _hg.setNodePart(hn, 0);
          } else {
            _hg.setNodePart(hn, 1);
          }
        }
      }
      // TODO(heuer): Why is this only needed in this case?
      _hg.initializeNumCutHyperedges();
    }

    _config.initial_partitioning.unassigned_part = unassigned_part;
    InitialPartitionerBase::recalculateBalanceConstraints(_config.initial_partitioning.epsilon);
    InitialPartitionerBase::rollbackToBestCut();
    InitialPartitionerBase::performFMRefinement();
  }

  void reset() {
    _start_nodes.clear();
    _visit.resetAllBitsToFalse();
    _hyperedge_in_queue.resetAllBitsToFalse();
    _pq.clear();
  }

  void insertNodeIntoPQ(const HypernodeID hn, const PartitionID target_part,
                        const bool updateGain = false) {
    // TODO(heuer): Why is this check necessary?
    if (_hg.partID(hn) != target_part) {
      if (!_pq.contains(hn, target_part)) {
        const Gain gain = GainComputation::calculateGain(_hg, hn, target_part);
        _pq.insert(hn, target_part, gain);

        if (!_pq.isEnabled(target_part) &&
            target_part != _config.initial_partitioning.unassigned_part) {
          _pq.enablePart(target_part);
        }

        ASSERT(_pq.contains(hn, target_part),
               "Hypernode " << hn << " isn't succesfully inserted into pq " << target_part << "!");
        ASSERT(_pq.isEnabled(target_part),
               "PQ " << target_part << " is disabled!");
      } else if (updateGain) {
        const Gain gain = GainComputation::calculateGain(_hg, hn, target_part);
        _pq.updateKey(hn, target_part, gain);
      }
    }
  }

  void insertAndUpdateNodesAfterMove(const HypernodeID hn, const PartitionID target_part,
                                     const bool insert = true, const bool delete_nodes = true) {
    if (delete_nodes) {
      deleteNodeInAllBucketQueues(hn);
    }
    GainComputation::deltaGainUpdate(_hg, _config, _pq, hn,
                                     _config.initial_partitioning.unassigned_part, target_part,
                                     _visit);
    // Pushing incident hypernode into bucket queue or update gain value
    // TODO(heuer): Shouldn't it be possible to do this within the deltaGainUpdate function?
    if (insert) {
      for (const HyperedgeID he : _hg.incidentEdges(hn)) {
        if (!_hyperedge_in_queue[target_part * _hg.numEdges() + he]) {
          for (const HypernodeID pin : _hg.pins(he)) {
            if (_hg.partID(pin) == _config.initial_partitioning.unassigned_part) {
              insertNodeIntoPQ(pin, target_part);
              ASSERT(_pq.contains(pin, target_part),
                     "PQ of partition " << target_part << " should contain hypernode " << pin << "!");
            }
          }
          _hyperedge_in_queue.setBit(target_part * _hg.numEdges() + he, true);
        }
      }
    }

    if(!_pq.isEnabled(target_part)) {
    	insertUnassignedHypernodeIntoPQ(target_part);
    }


    ASSERT([&]() {
        for (const HyperedgeID he : _hg.incidentEdges(hn)) {
          for (const HypernodeID pin : _hg.pins(he)) {
            for (PartitionID i = 0; i < _config.initial_partitioning.k; ++i) {
              if (_pq.isEnabled(i) && _pq.contains(pin, i)) {
                const Gain gain = GainComputation::calculateGain(_hg, pin, i);
                if (gain != _pq.key(pin, i)) {
                  return false;
                }
              }
            }
          }
        }
        return true;
      } (),
           "Gain value of a move of a hypernode isn't equal with the real gain.");
  }

  void deleteNodeInAllBucketQueues(const HypernodeID hn) {
    for (PartitionID part = 0; part < _config.initial_partitioning.k; ++part) {
      if (_pq.contains(hn, part)) {
    	if(_pq.isEnabled(part) && _pq.size(part) == 1 && _hg.partID(hn) != part) {
    		insertUnassignedHypernodeIntoPQ(part);
    	}
        _pq.remove(hn, part);
      }
    }
    ASSERT(!_pq.contains(hn),
           "Hypernode " << hn << " isn't succesfully deleted from all PQs.");
  }

  void insertUnassignedHypernodeIntoPQ(PartitionID part) {
	 HypernodeID unassigned_node = InitialPartitionerBase::getUnassignedNode();
	 if (unassigned_node != kInvalidNode) {
	   insertNodeIntoPQ(unassigned_node, part);
	 }
  }


  void calculateStartNodes() {
    // TODO(heuer): This again seems unnecessary.. why use the start_nodes vector
    // when you could directly insert into PQ?
    StartNodeSelection::calculateStartNodes(_start_nodes, _hg,
                                            _config.initial_partitioning.k);

    const int start_node_size = _start_nodes.size();

    for (PartitionID i = 0; i < start_node_size; i++) {
      insertNodeIntoPQ(_start_nodes[i], i);
    }

    ASSERT([&]() {
    	std::sort(_start_nodes.begin(),_start_nodes.end());
    	return std::unique(_start_nodes.begin(),_start_nodes.end()) == _start_nodes.end();
      } (), "There are at least two start nodes which are equal!");
  }

// double max_net_size;
  using InitialPartitionerBase::_hg;
  using InitialPartitionerBase::_config;
  // TODO(heuer): get rid of start nodes vector.
  std::vector<HypernodeID> _start_nodes;
  KWayRefinementPQ _pq;
  FastResetBitVector<> _visit;
  FastResetBitVector<> _hyperedge_in_queue;

  static const Gain kInvalidGain = std::numeric_limits<Gain>::min();
  static const PartitionID kInvalidPartition = -1;
  static const HypernodeID kInvalidNode =
    std::numeric_limits<HypernodeID>::max();
};
}

#endif  /* SRC_PARTITION_INITIAL_PARTITIONING_GREEDYHYPERGRAPHGROWINGINITIALPARTITIONER_H_ */
