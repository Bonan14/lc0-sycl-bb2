/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "mcts/node.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <list>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "utils/exception.h"
#include "utils/hashcat.h"
#include "utils/numa.h"

namespace lczero {

/////////////////////////////////////////////////////////////////////////
// Node garbage collector
/////////////////////////////////////////////////////////////////////////

namespace {
// Periodicity of garbage collection, milliseconds.
const int kGCIntervalMs = 100;

// Every kGCIntervalMs milliseconds release nodes in a separate GC thread.
class NodeGarbageCollector {
 public:
  NodeGarbageCollector() : gc_thread_([this]() { Worker(); }) {}

  // Takes ownership of a subtree, to dispose it in a separate thread when
  // it has time.
  void AddToGcQueue(std::unique_ptr<Node> node) {
    if (!node) return;
    Mutex::Lock lock(gc_mutex_);
    subtrees_to_gc_.emplace_back(std::move(node));
  }

  ~NodeGarbageCollector() {
    // Flips stop flag and waits for a worker thread to stop.
    stop_.store(true);
    gc_thread_.join();
  }

 private:
  void GarbageCollect() {
    while (!stop_.load()) {
      // Node will be released in destructor when mutex is not locked.
      std::unique_ptr<Node> node_to_gc;
      {
        // Lock the mutex and move last subtree from subtrees_to_gc_ into
        // node_to_gc.
        Mutex::Lock lock(gc_mutex_);
        if (subtrees_to_gc_.empty()) return;
        node_to_gc = std::move(subtrees_to_gc_.back());
        subtrees_to_gc_.pop_back();

        node_to_gc->UnsetLowNode();
      }
    }
  }

  void Worker() {
    // Keep garbage collection on same core as where search workers are most
    // likely to be to make any lock conention on gc mutex cheaper.
    Numa::BindThread(0);
    while (!stop_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kGCIntervalMs));
      GarbageCollect();
    };
  }

  mutable Mutex gc_mutex_;
  std::vector<std::unique_ptr<Node>> subtrees_to_gc_ GUARDED_BY(gc_mutex_);

  // When true, Worker() should stop and exit.
  std::atomic<bool> stop_{false};
  std::thread gc_thread_;
};

NodeGarbageCollector gNodeGc;
}  // namespace

/////////////////////////////////////////////////////////////////////////
// Edge
/////////////////////////////////////////////////////////////////////////

Move Edge::GetMove(bool as_opponent) const {
  if (!as_opponent) return move_;
  Move m = move_;
  m.Mirror();
  return m;
}

// Policy priors (P) are stored in a compressed 16-bit format.
//
// Source values are 32-bit floats:
// * bit 31 is sign (zero means positive)
// * bit 30 is sign of exponent (zero means nonpositive)
// * bits 29..23 are value bits of exponent
// * bits 22..0 are significand bits (plus a "virtual" always-on bit: s ∈ [1,2))
// The number is then sign * 2^exponent * significand, usually.
// See https://www.h-schmidt.net/FloatConverter/IEEE754.html for details.
//
// In compressed 16-bit value we store bits 27..12:
// * bit 31 is always off as values are always >= 0
// * bit 30 is always off as values are always < 2
// * bits 29..28 are only off for values < 4.6566e-10, assume they are always on
// * bits 11..0 are for higher precision, they are dropped leaving only 11 bits
//     of precision
//
// When converting to compressed format, bit 11 is added to in order to make it
// a rounding rather than truncation.
//
// Out of 65556 possible values, 2047 are outside of [0,1] interval (they are in
// interval (1,2)). This is fine because the values in [0,1] are skewed towards
// 0, which is also exactly how the components of policy tend to behave (since
// they add up to 1).

// If the two assumed-on exponent bits (3<<28) are in fact off, the input is
// rounded up to the smallest value with them on. We accomplish this by
// subtracting the two bits from the input and checking for a negative result
// (the subtraction works despite crossing from exponent to significand). This
// is combined with the round-to-nearest addition (1<<11) into one op.
void Edge::SetP(float p) {
  assert(0.0f <= p && p <= 1.0f);
  constexpr int32_t roundings = (1 << 11) - (3 << 28);
  int32_t tmp;
  std::memcpy(&tmp, &p, sizeof(float));
  tmp += roundings;
  p_ = (tmp < 0) ? 0 : static_cast<uint16_t>(tmp >> 12);
}

float Edge::GetP() const {
  // Reshift into place and set the assumed-set exponent bits.
  uint32_t tmp = (static_cast<uint32_t>(p_) << 12) | (3 << 28);
  float ret;
  std::memcpy(&ret, &tmp, sizeof(uint32_t));
  return ret;
}

std::string Edge::DebugString() const {
  std::ostringstream oss;
  oss << "Move: " << move_.as_string() << " p_: " << p_ << " GetP: " << GetP();
  return oss.str();
}

std::unique_ptr<Edge[]> Edge::FromMovelist(const MoveList& moves) {
  std::unique_ptr<Edge[]> edges = std::make_unique<Edge[]>(moves.size());
  auto* edge = edges.get();
  for (const auto move : moves) edge++->move_ = move;
  return edges;
}

/////////////////////////////////////////////////////////////////////////
// LowNode + Node
/////////////////////////////////////////////////////////////////////////

void LowNode::CopyPolicy(int max_needed, float* output) const {
  if (num_edges_ == 0) return;
  int loops = std::min(static_cast<int>(num_edges_), max_needed);
  for (int i = 0; i < loops; i++) {
    output[i] = edges_[i].GetP();
  }
}

float Node::GetVisitedPolicy() const {
  float sum = 0.0f;
  for (auto* node : VisitedNodes()) sum += GetEdgeToNode(node)->GetP();
  return sum;
}

Edge* LowNode::GetEdgeToNode(const Node* node) const {
  assert(node->GetParent() == this);
  assert(node->Index() < num_edges_);
  return &edges_[node->Index()];
}

std::string Node::DebugString() const {
  std::ostringstream oss;
  oss << " <Node> This:" << this << " LowNode:" << low_node_.get()
      << " Parent:" << parent_ << " Index:" << index_
      << " Sibling:" << sibling_.get() << " WL:" << wl_ << " D:" << d_
      << " M:" << m_ << " N:" << n_ << " N_:" << n_in_flight_
      << " Term:" << static_cast<int>(terminal_type_)
      << " Bounds:" << static_cast<int>(lower_bound_) - 2 << ","
      << static_cast<int>(upper_bound_) - 2;
  return oss.str();
}

std::string LowNode::DebugString() const {
  std::ostringstream oss;
  oss << " <LowNode> This:" << this << " Edges:" << edges_.get()
      << " NumEdges:" << static_cast<int>(num_edges_)
      << " Child:" << child_.get() << " WL:" << wl_ << " D:" << d_
      << " M:" << m_ << " N:" << n_
      << " NP:" << static_cast<int>(num_parents_)
      << " Term:" << static_cast<int>(terminal_type_)
      << " Bounds:" << static_cast<int>(lower_bound_) - 2 << ","
      << static_cast<int>(upper_bound_) - 2;
  return oss.str();
}

void Edge::SortEdges(Edge* edges, int num_edges) {
  // Sorting on raw p_ is the same as sorting on GetP() as a side effect of
  // the encoding, and its noticeably faster.
  std::sort(edges, (edges + num_edges),
            [](const Edge& a, const Edge& b) { return a.p_ > b.p_; });
}

void LowNode::MakeTerminal(GameResult result, float plies_left, Terminal type) {
  if (type != Terminal::TwoFold) SetBounds(result, result);
  terminal_type_ = type;
  m_ = plies_left;
  if (result == GameResult::DRAW) {
    wl_ = 0.0f;
    d_ = 1.0f;
  } else if (result == GameResult::WHITE_WON) {
    wl_ = 1.0f;
    d_ = 0.0f;
  } else if (result == GameResult::BLACK_WON) {
    wl_ = -1.0f;
    d_ = 0.0f;
  }
}

void LowNode::MakeNotTerminal(const Node* node) {
  assert(edges_);
  if (!IsTerminal()) return;

  terminal_type_ = Terminal::NonTerminal;
  lower_bound_ = GameResult::BLACK_WON;
  upper_bound_ = GameResult::WHITE_WON;
  n_ = 0;
  wl_ = 0.0;
  d_ = 0.0;
  m_ = 0.0;

  // Include children too.
  if (node->GetNumEdges() > 0) {
    for (const auto& child : node->Edges()) {
      const auto n = child.GetN();
      if (n > 0) {
        n_ += n;
        // Flip Q for opponent.
        // Default values don't matter as n is > 0.
        wl_ += child.GetWL(0.0f) * n;
        d_ += child.GetD(0.0f) * n;
        m_ += child.GetM(0.0f) * n;
      }
    }

    // Recompute with current eval (instead of network's) and children's eval.
    wl_ /= n_;
    d_ /= n_;
    m_ /= n_;
  }
}

void LowNode::SetBounds(GameResult lower, GameResult upper) {
  lower_bound_ = lower;
  upper_bound_ = upper;
}

void Node::MakeTerminal(GameResult result, float plies_left, Terminal type) {
  if (type != Terminal::TwoFold) SetBounds(result, result);
  terminal_type_ = type;
  m_ = plies_left;
  if (result == GameResult::DRAW) {
    wl_ = 0.0f;
    d_ = 1.0f;
  } else if (result == GameResult::WHITE_WON) {
    wl_ = 1.0f;
    d_ = 0.0f;
  } else if (result == GameResult::BLACK_WON) {
    wl_ = -1.0f;
    d_ = 0.0f;
    // Terminal losses have no uncertainty and no reason for their U value to be
    // comparable to another non-loss choice. Force this by clearing the policy.
    if (GetParent() != nullptr) GetOwnEdge()->SetP(0.0f);
  }
}

void Node::MakeNotTerminal(bool also_low_node) {
  // At least one of node and low node pair needs to be a terminal.
  if (!IsTerminal() &&
      (!also_low_node || !low_node_ || !low_node_->IsTerminal()))
    return;

  terminal_type_ = Terminal::NonTerminal;
  if (low_node_) {  // Two-fold or derived terminal.
    // Revert low node first.
    if (also_low_node && low_node_) low_node_->MakeNotTerminal(this);

    auto [lower_bound, upper_bound] = low_node_->GetBounds();
    lower_bound_ = -upper_bound;
    upper_bound_ = -lower_bound;
    n_ = low_node_->GetN();
    wl_ = -low_node_->GetWL();
    d_ = low_node_->GetD();
    m_ = low_node_->GetM() + 1;
  } else {  // Real terminal.
    lower_bound_ = GameResult::BLACK_WON;
    upper_bound_ = GameResult::WHITE_WON;
    n_ = 0.0f;
    wl_ = 0.0f;
    d_ = 0.0f;
    m_ = 0.0f;
  }
}

void Node::SetBounds(GameResult lower, GameResult upper) {
  lower_bound_ = lower;
  upper_bound_ = upper;
}

bool Node::TryStartScoreUpdate() {
  if (n_ == 0 && n_in_flight_ > 0) return false;
  ++n_in_flight_;
  return true;
}

void Node::CancelScoreUpdate(int multivisit) {
  assert(n_in_flight_ >= (uint32_t)multivisit);
  n_in_flight_ -= multivisit;
}

void LowNode::FinalizeScoreUpdate(float v, float d, float m, int multivisit) {
  assert(edges_);
  // Recompute Q.
  wl_ += multivisit * (v - wl_) / (n_ + multivisit);
  d_ += multivisit * (d - d_) / (n_ + multivisit);
  m_ += multivisit * (m - m_) / (n_ + multivisit);

  // Increment N.
  n_ += multivisit;
}

void LowNode::AdjustForTerminal(float v, float d, float m, int multivisit) {
  // Recompute Q.
  wl_ += multivisit * v / n_;
  d_ += multivisit * d / n_;
  m_ += multivisit * m / n_;
}

void Node::FinalizeScoreUpdate(float v, float d, float m, int multivisit) {
  // Recompute Q.
  wl_ += multivisit * (v - wl_) / (n_ + multivisit);
  d_ += multivisit * (d - d_) / (n_ + multivisit);
  m_ += multivisit * (m - m_) / (n_ + multivisit);

  // Increment N.
  n_ += multivisit;
  // Decrement virtual loss.
  assert(n_in_flight_ >= (uint32_t)multivisit);
  n_in_flight_ -= multivisit;
}

void Node::AdjustForTerminal(float v, float d, float m, int multivisit) {
  // Recompute Q.
  wl_ += multivisit * v / n_;
  d_ += multivisit * d / n_;
  m_ += multivisit * m / n_;
}

void Node::RevertTerminalVisits(float v, float d, float m, int multivisit) {
  // Compute new n_ first, as reducing a node to 0 visits is a special case.
  const int n_new = n_ - multivisit;
  if (n_new <= 0) {
    // If n_new == 0, reset all relevant values to 0.
    wl_ = 0.0;
    d_ = 1.0;
    m_ = 0.0;
    n_ = 0;
  } else {
    // Recompute Q and M.
    wl_ -= multivisit * (v - wl_) / n_new;
    d_ -= multivisit * (d - d_) / n_new;
    m_ -= multivisit * (m - m_) / n_new;
    // Decrement N.
    n_ -= multivisit;
  }
}

void LowNode::ReleaseChildren() { gNodeGc.AddToGcQueue(std::move(child_)); }

void LowNode::ReleaseChildrenExceptOne(Node* node_to_save) {
  // Stores node which will have to survive (or nullptr if it's not found).
  std::unique_ptr<Node> saved_node;
  // Pointer to unique_ptr, so that we could move from it.
  for (std::unique_ptr<Node>* node = &child_; *node;
       node = (*node)->GetSibling()) {
    // If current node is the one that we have to save.
    if (node->get() == node_to_save) {
      // Kill all remaining siblings.
      gNodeGc.AddToGcQueue(std::move(*(*node)->GetSibling()));
      // Save the node, and take the ownership from the unique_ptr.
      saved_node = std::move(*node);
      break;
    }
  }
  // Make saved node the only child. (kills previous siblings).
  gNodeGc.AddToGcQueue(std::move(child_));
  child_ = std::move(saved_node);
}

static std::string PtrToNodeName(const void* ptr) {
  std::ostringstream oss;
  oss << "n_" << ptr;
  return oss.str();
}

std::string LowNode::DotNodeString() const {
  std::ostringstream oss;
  oss << PtrToNodeName(this) << " ["
      << "shape=box";
  // Adjust formatting to limit node size.
  oss << std::fixed << std::setprecision(3);
  oss << ",label=\""     //
      << std::showpos    //
      << "WL=" << wl_    //
      << std::noshowpos  //
      << "\\lD=" << d_ << "\\lM=" << m_ << "\\lN=" << n_;
  // Set precision for tooltip.
  oss << std::fixed << std::showpos << std::setprecision(5);
  oss << ",tooltip=\""   //
      << std::showpos    //
      << "WL=" << wl_    //
      << std::noshowpos  //
      << "\\nD=" << d_ << "\\nM=" << m_ << "\\nN=" << n_
      << "\\nNP=" << static_cast<int>(num_parents_)
      << "\\nTerm=" << static_cast<int>(terminal_type_)  //
      << std::showpos                                    //
      << "\\nBounds=" << static_cast<int>(lower_bound_) - 2 << ","
      << static_cast<int>(upper_bound_) - 2  //
      << std::noshowpos                      //
      << "\\n\\nThis=" << this << "\\nEdges=" << edges_.get()
      << "\\nNumEdges=" << static_cast<int>(num_edges_)
      << "\\nChild=" << child_.get() << "\\n\"";
  oss << "];";
  return oss.str();
}

std::string Node::DotEdgeString(bool as_opponent) const {
  std::ostringstream oss;
  oss << (parent_ == nullptr ? "top" : PtrToNodeName(parent_)) << " -> "
      << (low_node_ ? PtrToNodeName(low_node_.get()) : PtrToNodeName(this))
      << " [";
  oss << "label=\""
      << (parent_ == nullptr ? "N/A"
                             : GetOwnEdge()->GetMove(as_opponent).as_string())
      << "\\lN=" << n_;
  if (IsTwoFoldTerminal()) {
    oss << "\\lDRAW";
  }
  oss << "\\l\"";
  // Set precision for tooltip.
  oss << std::fixed << std::setprecision(5);
  oss << ",labeltooltip=\""
      << "P=" << (parent_ == nullptr ? 0.0f : GetOwnEdge()->GetP())
      << std::showpos      //
      << "\\nWL= " << wl_  //
      << std::noshowpos    //
      << "\\nD=" << d_ << "\\nM=" << m_ << "\\nN=" << n_
      << "\\nTerm=" << static_cast<int>(terminal_type_)  //
      << std::showpos                                    //
      << "\\nBounds=" << static_cast<int>(lower_bound_) - 2 << ","
      << static_cast<int>(upper_bound_) - 2 << "\\n\\nThis=" << this  //
      << std::noshowpos                                               //
      << "\\nLowNode=" << low_node_.get() << "\\nParent=" << parent_
      << "\\nIndex=" << index_ << "\\nSibling=" << sibling_.get() << "\\n\"";
  oss << "];";
  return oss.str();
}

std::string Node::DotGraphString(bool as_opponent) const {
  std::ostringstream oss;
  std::unordered_set<const Node*> seen;
  std::list<std::pair<const Node*, bool>> unvisited_fifo;

  oss << "strict digraph {" << std::endl;
  oss << "edge ["
      << "headport=n"
      << ",tooltip=\" \""  // Remove default tooltips from edge parts.
      << "];" << std::endl;
  oss << "node ["
      << "shape=point"    // For fake nodes.
      << ",style=filled"  // Show tooltip everywhere on the node.
      << ",fillcolor=ivory"
      << "];" << std::endl;
  oss << "ranksep=" << 4.0f * std::log10(GetN()) << std::endl;

  oss << DotEdgeString(!as_opponent) << std::endl;
  unvisited_fifo.push_back(std::pair(this, as_opponent));
  seen.insert(this);

  do {
    auto [parent_node, parent_as_opponent] = unvisited_fifo.front();
    unvisited_fifo.pop_front();

    auto parent_low_node = parent_node->GetLowNode().get();
    if (parent_low_node) {
      oss << parent_low_node->DotNodeString() << std::endl;

      for (auto& child_edge : parent_node->Edges()) {
        auto child = child_edge.node();
        if (child == nullptr) break;

        oss << child->DotEdgeString(parent_as_opponent) << std::endl;

        if (seen.find(child) == seen.end()) {
          unvisited_fifo.push_back(std::pair(child, !parent_as_opponent));
          seen.insert(child);
        }
      }
    }
  } while (!unvisited_fifo.empty());

  oss << "}" << std::endl;

  return oss.str();
}

/////////////////////////////////////////////////////////////////////////
// EdgeAndNode
/////////////////////////////////////////////////////////////////////////

std::string EdgeAndNode::DebugString() const {
  if (!edge_) return "(no edge)";
  return edge_->DebugString() + " " +
         (node_ ? node_->DebugString() : "(no node)");
}

/////////////////////////////////////////////////////////////////////////
// NodeTree
/////////////////////////////////////////////////////////////////////////

void NodeTree::MakeMove(Move move) {
  if (HeadPosition().IsBlackToMove()) move.Mirror();
  const auto& board = HeadPosition().GetBoard();

  Node* new_head = nullptr;
  for (auto& n : current_head_->Edges()) {
    if (board.IsSameMove(n.GetMove(), move)) {
      new_head = n.GetOrSpawnNode(current_head_);
      // Ensure head is not terminal, so search can extend or visit children of
      // "terminal" positions, e.g., WDL hits, converted terminals, 3-fold draw.
      if (new_head->IsTerminal()) new_head->MakeNotTerminal();
      break;
    }
  }
  move = board.GetModernMove(move);
  current_head_->ReleaseChildrenExceptOne(new_head);
  new_head = current_head_->GetChild();
  current_head_ =
      new_head ? new_head : current_head_->CreateSingleChildNode(move);
  history_.Append(move);
  moves_.push_back(move);
}

void NodeTree::TrimTreeAtHead() {
  auto tmp = current_head_->MoveSiblingOut();
  // Send dependent nodes for GC instead of destroying them immediately.
  current_head_->ReleaseChildren();
  *current_head_ = Node(current_head_->GetParent(), current_head_->Index());
  current_head_->MoveSiblingIn(tmp);
}

bool NodeTree::ResetToPosition(const std::string& starting_fen,
                               const std::vector<Move>& moves) {
  ChessBoard starting_board;
  int no_capture_ply;
  int full_moves;
  starting_board.SetFromFen(starting_fen, &no_capture_ply, &full_moves);
  if (gamebegin_node_ &&
      (history_.Starting().GetBoard() != starting_board ||
       history_.Starting().GetRule50Ply() != no_capture_ply)) {
    // Completely different position.
    DeallocateTree();
  }

  if (!gamebegin_node_) {
    gamebegin_node_ = std::make_unique<Node>(static_cast<LowNode*>(nullptr), 0);
  }

  history_.Reset(starting_board, no_capture_ply,
                 full_moves * 2 - (starting_board.flipped() ? 1 : 2));
  moves_.clear();

  Node* old_head = current_head_;
  current_head_ = gamebegin_node_.get();
  bool seen_old_head = (gamebegin_node_.get() == old_head);
  for (const auto& move : moves) {
    MakeMove(move);
    if (old_head == current_head_) seen_old_head = true;
  }

  // MakeMove guarantees that no siblings exist; but, if we didn't see the old
  // head, it means we might have a position that was an ancestor to a
  // previously searched position, which means that the current_head_ might
  // retain old n_ and q_ (etc) data, even though its old children were
  // previously trimmed; we need to reset current_head_ in that case.
  if (!seen_old_head) TrimTreeAtHead();
  return seen_old_head;
}

void NodeTree::DeallocateTree() {
  // Same as gamebegin_node_.reset(), but actual deallocation will happen in
  // GC thread.
  gNodeGc.AddToGcQueue(std::move(gamebegin_node_));
  gamebegin_node_ = nullptr;
  current_head_ = nullptr;
}

}  // namespace lczero
