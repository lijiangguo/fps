//
// map.h
// 
// Copyright 2014 Vincent Q. Vu. All rights reserved
//

template <typename Map, typename bT = arma::mat>
struct map : public BlockMat<bT> {

  map(const bT& X, const Map& indexmap) {
    for (auto& i : indexmap) {
      bT b = X.submat(i.second.first, i.second.second);
      BlockMat<bT>::blocks.push_back(std::move(b));
    }
  }

  void copy_to(bT& X, const Map& indexmap) const {
    auto b = BlockMat<bT>::blocks.cbegin();
    for (auto& i : indexmap) { 
      X.submat(i.second.first, i.second.second) = *b++; 
    }
  }
};

template <typename Map, typename bT = arma::mat >
struct symmap : public BlockMat<bT> {

  symmap(const bT& X, const Map& indexmap) {
    for (auto& i : indexmap) {
      bT b = X.submat(i.second, i.second);
      BlockMat<bT>::blocks.push_back(std::move(b));
    }
  }

  void copy_to(bT& X, const Map& indexmap) const {
    auto b = BlockMat<bT>::blocks.cbegin();
    for (auto& i : indexmap) { 
      X.submat(i.second, i.second) = *b++; 
    }
  }
};
